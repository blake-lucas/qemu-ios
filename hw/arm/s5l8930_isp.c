/*
 * The A4's camera ISP ('isp,s5l8930x', AppleH3CameraInterface's AppleH3CamIn): a high-level model of the firmware
 * its ISP CPU runs, with a test pattern for a sensor.
 *
 * Two blocks. The ISP CPU's (DT isp reg[1], 0x88100000): reset and start at 0x100, the firmware's DART window at
 * 0x110/0x114 (mediaserverd loads the ARM firmware from H3ISPServices' __DATA,H3ISPFIRMWARE), and a mailbox: up to
 * seven words at 0x120 (category << 16 | register << 8 | value), 0x13c the status (bit 31 the host's direction,
 * 1 = write; bit 30 the firmware's done; bits 7..0 the valid words), 0x104 = 2 the doorbell. The firmware exposes
 * its state as a byte register file addressed by (category, register); the kext and H3ISP.mediacapture drive the
 * camera through it: sensor recognition, modes, frame sizes (03/04:17..1e, back/front: the sensor's, then the
 * output's), setfiles, AE/AWB/AF and the shutter. The model keeps the file and acts on the registers that are
 * commands (isp_command). The ISP's own block (reg[0], 0x88300000) is what the kext programs directly: the output
 * DMA (two buffers, current and next, latched at each frame's end), its format, and the frame-done interrupt
 * (status 0xb000c, line 8).
 *
 * Per frame the model writes the pattern into the current buffer through dart1 (DT mapper-isp, stream 1), the
 * statistics record into the metadata buffer the kext named (00:cb or 00:cf, alternating), names that buffer in
 * a2:78..7b, latches the next buffer and raises the interrupt. The still is the frame flagged with the shutter.
 * Checked on the iPhone 4's 4.2.1 (AppleH3CamIn 9.9.0) and the iPod touch 4G's 6.1.6 (9.44), back camera.
 * IT_ISP_TRACE=FILE logs the conversation.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/arm/s5l8930.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/bswap.h"

#define ISP_CPU_SIZE    0x1000
#define ISP_SIZE        0xd6000
#define ISP_OUT_MAX     4096        /* output width and height */

/* ISP CPU block */
#define CPU_CONTROL     0x100       /* 0x10 at ISP_ARMReset */
#define CPU_KICK        0x104       /* 2: a mailbox transaction is ready */
#define CPU_MBX0        0x120       /* words 0..6: (category << 16 | register << 8 | value) */
#define CPU_MBX_STATUS  0x13c       /* bit 31 host: write (1) / read (0), firmware: error; bit 30 done; 7..0 valid */

#define MBX_WRITE       (1u << 31)
#define MBX_DONE        (1u << 30)

/* ISP block */
#define ISP_INT_STATUS  0xb000c     /* W1C: 0x1000 frame done, 0x2000 overflow */
#define ISP_OUT_ENABLE  0xd06a0
#define ISP_CUR_Y       0xd3654     /* the buffer being filled: luma, chroma (0x1000 = none) */
#define ISP_CUR_C       0xd3658
#define ISP_NEXT_Y      0xd3660     /* latched into CUR at each frame's end */
#define ISP_NEXT_C      0xd3664
#define ISP_OUT_FORMAT  0xd3b20     /* 0x11: 4:2:0 biplanar (NV12), 0x10: 4:2:2 */

#define INT_FRAME       0x1000
#define ISP_DART_SID    1

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930ISPState, S5L8930_ISP)

struct S5L8930ISPState {
    SysBusDevice parent_obj;
    MemoryRegion cpu_mr, isp_mr;
    qemu_irq irq[2];                /* the ISP's (8); the ISP CPU's (9), never raised: the kext polls the mailbox */

    uint32_t cpu[ISP_CPU_SIZE / 4];
    uint32_t *isp;                  /* ISP_SIZE / 4 words */
    uint8_t file[256][256];         /* the firmware's category register file */

    DeviceState *dart;
    QEMUTimer *frame_timer;
    uint32_t frames;
    uint8_t shutter;                /* frames still to flag as the still (00:33 one, 00:34 preflash and flash) */
    uint8_t strobe;                 /* the flagged frame's strobe capture count */
    uint8_t *pic;                   /* one frame, host side */
    size_t pic_size;

    FILE *trace;
};

void s5l8930_isp_set_dart(DeviceState *isp, DeviceState *dart)
{
    S5L8930_ISP(isp)->dart = dart;
}

static void isp_log(S5L8930ISPState *s, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void isp_log(S5L8930ISPState *s, const char *fmt, ...)
{
    va_list ap;

    if (!s->trace) {
        return;
    }
    va_start(ap, fmt);
    fprintf(s->trace, "%" PRId64 " ", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    vfprintf(s->trace, fmt, ap);
    fputc('\n', s->trace);
    va_end(ap);
}

/*
 * Command registers: the host writes a request and polls the register until the firmware clears it (results, if
 * any, in neighboring registers). The firmware is done at once here.
 */
static const struct { uint8_t cat, reg; } isp_commands[] = {
    { 0x00, 0x0a },     /* mode change (1 preview, 2 bypass, 3 both) */
    { 0x00, 0x40 },     /* setfile load from shared memory */
    { 0x00, 0xca },
    { 0x03, 0x00 },     /* sensor recognition, back (MIPI): result in 03:07, 0 = found */
    { 0x04, 0x00 },     /* sensor recognition, front (SMIA) */
    { 0x03, 0x71 },
    { 0x04, 0x71 },
    { 0x24, 0xb4 },     /* sensor I2C transaction: 24:b5 slave, b6/b7 register/data, result in 24:b9 */
};

static void isp_command(S5L8930ISPState *s, uint8_t cat, uint8_t reg)
{
    if (cat == 0x24 && reg == 0x0c && s->file[cat][reg] == 5) {
        s->file[cat][reg] = 1;                          /* request 5 done: the host polls for 1 */
        return;
    }
    if (cat == 0x00 && (reg == 0x33 || reg == 0x34) && s->file[cat][reg]) {
        /* Shutter (33) or shutter+strobe (34): the next frame is the still; with the strobe, a preflash frame
         * (strobe capture count 1) comes first and the still is the flash frame (2). */
        s->file[cat][reg] = 0;
        s->shutter = reg == 0x33 ? 1 : 2;
        s->strobe = reg == 0x33 ? 0 : 1;
        return;
    }
    if (cat == 0x00 && reg == 0x00) {
        s->file[0x00][0x91] = s->file[0x00][0x00];      /* requested sensor mode -> current mode */
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(isp_commands); i++) {
        if (isp_commands[i].cat == cat && isp_commands[i].reg == reg) {
            s->file[cat][reg] = 0;
            if ((cat == 0x03 || cat == 0x04) && reg == 0x00) {
                s->file[cat][0x07] = 0;
            }
            return;
        }
    }
}

static void isp_mailbox(S5L8930ISPState *s)
{
    uint32_t st = s->cpu[CPU_MBX_STATUS / 4];
    bool write = st & MBX_WRITE;

    for (unsigned i = 0; i < 7; i++) {
        if (!(st & (1u << i))) {
            continue;
        }
        uint32_t w = s->cpu[CPU_MBX0 / 4 + i];
        uint8_t cat = (w >> 16) & 0xff, reg = (w >> 8) & 0xff, val = w;   /* file[] is 256 x 256 */

        if (write) {
            s->file[cat][reg] = val;
            isp_log(s, "mbx W %02x:%02x = %02x", cat, reg, val);
            isp_command(s, cat, reg);
        } else {
            s->cpu[CPU_MBX0 / 4 + i] = (w & 0xffff00) | s->file[cat][reg];
            isp_log(s, "mbx R %02x:%02x = %02x", cat, reg, s->file[cat][reg]);
        }
    }
    s->cpu[CPU_MBX_STATUS / 4] = (st & ~MBX_WRITE) | MBX_DONE;
}

static uint64_t isp_cpu_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930ISPState *s = opaque;

    return s->cpu[addr / 4];
}

static void isp_cpu_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930ISPState *s = opaque;

    if (addr != CPU_MBX_STATUS && (addr < CPU_MBX0 || addr >= CPU_MBX_STATUS)) {
        isp_log(s, "cpu W %03x = %08x", (unsigned)addr, (uint32_t)val);
    }
    s->cpu[addr / 4] = val;
    if (addr == CPU_CONTROL && (val & 1)) {
        /* The firmware is up: its first mailbox completion is what ISP_InitDual waits for. */
        s->cpu[CPU_MBX_STATUS / 4] |= MBX_DONE;
    }
    if (addr == CPU_KICK && val == 2) {
        isp_mailbox(s);
    }
}

static const MemoryRegionOps isp_cpu_ops = {
    .read = isp_cpu_read,
    .write = isp_cpu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void isp_update_irq(S5L8930ISPState *s)
{
    qemu_set_irq(s->irq[0], (s->isp[ISP_INT_STATUS / 4] & 0x3000) != 0);
}

/* Write `len` bytes at ISP bus address `va` through dart1, a page at a time. */
static void isp_dma_write(S5L8930ISPState *s, uint32_t va, const uint8_t *buf, size_t len)
{
    while (len) {
        size_t n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = s->dart ? s5l8930_dart_dev_xlate(s->dart, ISP_DART_SID, va) : va;

        if (pa != (hwaddr)-1) {
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, buf, n);
        }
        va += n;
        buf += n;
        len -= n;
    }
}

/*
 * The sensor: a test pattern. Eight vertical color bars (BT.601 video range, as the ISP's YCbCr output) and a
 * white bar sweeping down the picture, a frame at a time, so a live viewfinder shows it moving.
 */
static void isp_pattern(S5L8930ISPState *s, unsigned w, unsigned h, bool nv12)
{
    static const uint8_t bars[8][3] = {     /* Y, Cb, Cr: white, yellow, cyan, green, magenta, red, blue, black */
        { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
        { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
    };
    unsigned sweep = (s->frames * 8) % h, band = MAX(h / 32, 2);
    uint8_t *p = s->pic;

    for (unsigned y = 0; y < h; y++) {
        bool lit = y - sweep < band;

        for (unsigned x = 0; x < w; x++) {
            const uint8_t *c = bars[x * 8 / w];

            if (nv12) {
                p[y * w + x] = lit ? 235 : c[0];
            } else {
                uint8_t *q = p + (y * w + x) * 2;
                q[0] = lit ? 235 : c[0];            /* 'yuvs': Y0 Cb Y1 Cr */
                q[1] = (x & 1) ? c[2] : c[1];
            }
        }
    }
    if (nv12) {
        uint8_t *uv = p + w * h;

        for (unsigned y = 0; y < h / 2; y++) {
            for (unsigned x = 0; x < w; x += 2) {
                const uint8_t *c = bars[x * 8 / w];

                uv[y * w + x] = c[1];
                uv[y * w + x + 1] = c[2];
            }
        }
    }
}

/*
 * The frame's statistics record (0x1a0 bytes; the kext copies it into H3ISPFrameMetaData), as H3ISP.mediacapture
 * reads it: AE converged at unity gains, AWB and AF settled, and the shutter flag that marks a still.
 * ponytail: the AE/AWB/AF numbers are constants and vSyncTime (0xcc) is 0; derive them from the pattern and
 * the timebase if a client starts using them.
 */
static void isp_metadata(S5L8930ISPState *s, uint32_t va)
{
    uint8_t md[0x1a0] = { 0 };

    stw_le_p(&md[0x74], 0x100);     /* aeAGC */
    stw_le_p(&md[0x76], 0x100);     /* aeDGain */
    stw_le_p(&md[0x78], 0x100);     /* sensorDGain */
    stw_le_p(&md[0x7a], 0x80);      /* aeAvg */
    md[0x7c] = 1;                   /* aeStable */
    md[0x86] = 1;                   /* awbStable */
    md[0xc0] = 0;                   /* afStatus */
    md[0xc2] = 1;                   /* afStable */
    if (s->shutter) {
        md[0xc5] = 1;               /* shutter: this frame is the still */
        stw_le_p(&md[0x126], s->strobe);
        s->shutter--;
        s->strobe += !!s->strobe;
    }
    if (va) {
        isp_dma_write(s, va, md, sizeof(md));
    }
}

static void isp_frame(void *opaque)
{
    S5L8930ISPState *s = opaque;
    uint32_t *r = s->isp;
    bool front = false;     /* ponytail: back sensor only; the front's selection is not decoded yet */
    uint8_t *f = s->file[front ? 0x04 : 0x03];
    unsigned w = f[0x1b] << 8 | f[0x1c], h = f[0x1d] << 8 | f[0x1e];     /* the output's size; 17..1a the sensor's */
    bool nv12 = r[ISP_OUT_FORMAT / 4] == 0x11;
    uint32_t y = r[ISP_CUR_Y / 4], c = r[ISP_CUR_C / 4];
    uint8_t *meta = s->file[front ? 0xa3 : 0xa2];
    const uint8_t *m = &s->file[0x00][(s->frames & 1) ? 0xcf : 0xcb];
    size_t need = (size_t)w * h * 2;

    if (!r[ISP_OUT_ENABLE / 4] || !y) {
        return;     /* stopped: no buffer, no frame */
    }
    /* The size is mailbox bytes the guest wrote: no larger than a sensor (5 MP) makes. */
    if (w && h && w <= ISP_OUT_MAX && h <= ISP_OUT_MAX && y > 0x1000) {
        if (need > s->pic_size) {
            s->pic = g_realloc(s->pic, need);
            s->pic_size = need;
        }
        isp_pattern(s, w, h, nv12);
        if (nv12) {
            isp_dma_write(s, y, s->pic, (size_t)w * h);
            if (c > 0x1000) {
                isp_dma_write(s, c, s->pic + w * h, (size_t)w * h / 2);
            }
        } else {
            isp_dma_write(s, y, s->pic, (size_t)w * h * 2);
        }
    }
    s->frames++;
    /* The finished frame's statistics, in the metadata buffer named by 00:cb (even frames) or 00:cf (odd). */
    memcpy(&meta[0x78], m, 4);   /* a2/a3:58 stays 0: nonzero is the firmware still writing them */
    isp_metadata(s, (m[0] << 24 | m[1] << 16 | m[2] << 8 | m[3]) & 0x0fffffff);
    r[ISP_CUR_Y / 4] = r[ISP_NEXT_Y / 4];
    r[ISP_CUR_C / 4] = r[ISP_NEXT_C / 4];
    r[ISP_INT_STATUS / 4] |= INT_FRAME;
    isp_update_irq(s);
    isp_log(s, "frame %u %ux%u %s -> %08x", s->frames, w, h, nv12 ? "nv12" : "422", y);
    timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 15);
}

static uint64_t isp_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930ISPState *s = opaque;

    return s->isp[addr / 4];
}

static void isp_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    S5L8930ISPState *s = opaque;

    isp_log(s, "isp W %05x = %08x", (unsigned)addr, (uint32_t)val);
    if (addr == ISP_INT_STATUS) {
        s->isp[addr / 4] &= ~val;
        isp_update_irq(s);
        return;
    }
    s->isp[addr / 4] = val;
    if (addr == ISP_OUT_ENABLE) {
        if (val & 1) {
            timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 15);
        } else {
            timer_del(s->frame_timer);
        }
    }
}

static const MemoryRegionOps isp_ops = {
    .read = isp_read,
    .write = isp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void isp_init(Object *obj)
{
    S5L8930ISPState *s = S5L8930_ISP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    const char *trace = getenv("IT_ISP_TRACE");

    s->isp = g_new0(uint32_t, ISP_SIZE / 4);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isp_frame, s);
    memory_region_init_io(&s->isp_mr, obj, &isp_ops, s, "s5l8930.isp", ISP_SIZE);
    memory_region_init_io(&s->cpu_mr, obj, &isp_cpu_ops, s, "s5l8930.isp-cpu", ISP_CPU_SIZE);
    sysbus_init_mmio(sbd, &s->isp_mr);
    sysbus_init_mmio(sbd, &s->cpu_mr);
    sysbus_init_irq(sbd, &s->irq[0]);
    sysbus_init_irq(sbd, &s->irq[1]);
    if (trace && *trace) {
        s->trace = fopen(trace, "a");
        if (s->trace) {
            setvbuf(s->trace, NULL, _IOLBF, 0);
        }
    }
}

static void isp_reset(DeviceState *dev)
{
    S5L8930ISPState *s = S5L8930_ISP(dev);

    memset(s->cpu, 0, sizeof(s->cpu));
    memset(s->isp, 0, ISP_SIZE);
    memset(s->file, 0, sizeof(s->file));
    timer_del(s->frame_timer);
    s->frames = 0;
    s->shutter = s->strobe = 0;
}

static const VMStateDescription isp_vmstate = {
    .name = TYPE_S5L8930_ISP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(cpu, S5L8930ISPState, ISP_CPU_SIZE / 4),
        VMSTATE_BUFFER_POINTER_UNSAFE(isp, S5L8930ISPState, 0, ISP_SIZE),
        VMSTATE_BUFFER_UNSAFE(file, S5L8930ISPState, 0, sizeof(((S5L8930ISPState *)0)->file)),
        VMSTATE_TIMER_PTR(frame_timer, S5L8930ISPState),
        VMSTATE_UINT32(frames, S5L8930ISPState),
        VMSTATE_UINT8(shutter, S5L8930ISPState),
        VMSTATE_UINT8(strobe, S5L8930ISPState),
        VMSTATE_END_OF_LIST()
    },
};

static void isp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, isp_reset);
    dc->vmsd = &isp_vmstate;
    dc->desc = "A4 camera ISP (H3) and its ISP CPU's firmware, high level";
}

static const TypeInfo isp_info = {
    .name = TYPE_S5L8930_ISP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930ISPState),
    .instance_init = isp_init,
    .class_init = isp_class_init,
};

static void isp_register_types(void)
{
    type_register_static(&isp_info);
}

type_init(isp_register_types)
