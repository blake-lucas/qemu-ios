#include "qemu/osdep.h"
#include "exec/cpu-common.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/thread.h"
#include "hw/arm/gles-present.h"
#include "system/physmem.h"
#include "system/ram_addr.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include "trace.h"

/* 7E18 AppleM2ScalerCSCDriver: 0xc0732388 programs formats, 0xc07323f0
 * source geometry, 0xc073246c destination geometry, 0xc0730288 acknowledges
 * completion. The device-tree scaler node supplies VIC interrupt 0x25. */
typedef struct {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x1000 / 4];
    /* iPad: buffers are dart2 IOVAs (DT mapper-scaler, stream 2). */
    hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid);
    void *xlate_opaque;
    unsigned sid;
    uint32_t version;   /* +0x260, the block's version: 0 on the S5L8720 (no tiled buffers) */
    QEMUTimer *done;    /* the running transfer's end */
    /*
     * Frame-sized work buffers, kept from one transfer to the next: allocated per transfer, each (2.4 MiB for a
     * 640x960 frame) was on Android a fresh mapping, faulted in a page at a time and unmapped again, every frame.
     */
    void *scratch[2];
    size_t scratch_size[2];
    QemuThread worker;
    QemuMutex lock;
    QemuCond cond;
    bool worker_up, queued, busy, on_worker;
    uint32_t job[0x1000 / 4];
    /* a second thread takes half of a large RGB transfer (scaler_split) */
    QemuThread helper;
    QemuCond helper_go, helper_done;
    bool helper_up;
    unsigned helper_gen, helper_seen, helper_finished;
    struct ScalerRgbJob *helper_job;
    int helper_phase;
} IPodScalerState;

void ipod_scaler_set_iommu(DeviceState *dev,
                           hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid),
                           void *opaque, unsigned sid);
void ipod_scaler_set_iommu(DeviceState *dev,
                           hwaddr (*xlate)(void *opaque, uint32_t va, unsigned sid),
                           void *opaque, unsigned sid)
{
    IPodScalerState *s = (IPodScalerState *)dev;

    s->xlate = xlate;
    s->xlate_opaque = opaque;
    s->sid = sid;
}

/*
 * AppleM2ScalerCSCDriver reads +0x260 once at start (8F190 0x80a45696, kept at driver+0x34) and gates features
 * on it: tiled buffers from 0x20002, more capabilities past 0x20006 and past 0x40006. 4.3's QuartzCore scales an
 * EAGL layer from a tiled surface, which version 0 refuses ("Scaler block (version = 0x0) for this chip does not
 * support tiled buffers"). scaler_row addresses a tiled buffer's tiles; their pixels are linear, as the host writes them.
 */
void ipod_scaler_set_version(DeviceState *dev, uint32_t version);
void ipod_scaler_set_version(DeviceState *dev, uint32_t version)
{
    IPodScalerState *s = (IPodScalerState *)dev;

    s->version = version;
    s->regs[0x260 / 4] = version;
}

static void *scaler_scratch(IPodScalerState *s, unsigned i, size_t size)
{
    if (s->scratch_size[i] < size) {
        g_free(s->scratch[i]);
        s->scratch[i] = g_malloc(size);
        s->scratch_size[i] = size;
    }
    return s->scratch[i];
}

static hwaddr scaler_pa(IPodScalerState *s, uint32_t va)
{
    return s->xlate ? s->xlate(s->xlate_opaque, va, s->sid) : va;
}

/* Write `len` bytes at a bus address (an IOVA behind the IOMMU), a page at a
 * time. False if any page is unmapped. */
static bool scaler_write_bus(IPodScalerState *s, uint32_t va, const void *buf,
                             unsigned len)
{
    const uint8_t *p = buf;

    while (len) {
        unsigned n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = scaler_pa(s, va);

        if (pa == (hwaddr)-1) {
            return false;
        }
        cpu_physical_memory_write(pa, p, n);
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

/*
 * The bus address of row `y` of a buffer programmed at `stride` bytes a row (+0x1c/+0x3c). A tiled buffer (+0x10/+0x30
 * bit 10, which AppleM2ScalerCSCHal sets from the request's isSourceTiled/isDestTiled: 9B206 0x80a4e286) is a grid
 * of 4 KiB tiles, 16 rows of 256 bytes each, a power of two of them across (potTilesX; the stride is potTilesX * 256
 * bytes), so a row of tiles is 16 strides long; the kernel maps the surface's own pages a tile apiece through the DART
 * from the start of each row of tiles and leaves the padding past them unmapped (isTiledAddressSourceMisaligned's
 * potTilesX, potTilesY and PAGE_SIZE). The tile pages hold the surface as the CPU sees it, which is how the GL bridge
 * writes it: rows of `pitch` = tilesX * 256 bytes, linear across a row of tiles. A 1x iPad app's 2x destination
 * (960x640 at 4096: 15 of each 16 pages; issue 47), the iPad 3.2 Zoom source (11 of 16 at 683 wide) and 5.1.1's 2x
 * source (320x480 at 2048: 5 of 8; issue 96) are tiled; a linear buffer is read at its stride, whatever the stride.
 */
static uint32_t scaler_row(uint32_t base, unsigned y, unsigned stride, unsigned pitch, bool tiled)
{
    return tiled ? base + y / 16 * 16 * stride + y % 16 * pitch : base + y * stride;
}

/* Read `len` bytes at a bus address, a page at a time. False if any page is unmapped. */
static bool scaler_read_bus(IPodScalerState *s, uint32_t va, void *buf, unsigned len)
{
    uint8_t *p = buf;

    while (len) {
        unsigned n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = scaler_pa(s, va);

        if (pa == (hwaddr)-1) {
            return false;
        }
        cpu_physical_memory_read(pa, p, n);
        va += n;
        p += n;
        len -= n;
    }
    return true;
}

/*
 * 32-bit RGB to 32-bit RGB with scaling: what Accessibility > Zoom asks for.
 * CA renders the visible crop (683x512 at 1.5x on the iPad) and the scaler
 * blows it up onto the framebuffer. +0x10/+0x30 formats (low 3 bits 6 =
 * 32 bpp, same layout both sides, so pixels are copied as they are), +0x14
 * source, +0x1c source stride in pixels, +0x24 source w << 16 | h, +0x34
 * destination, +0x3c destination stride in pixels, +0x40 destination
 * w << 16 | h, +0x50/+0x54 x/y step in 16.16.
 *
 * Both sides are addressed the same way (scaler_row), tiled or linear as
 * their format says. The iPad 3.2 Zoom source is tiled; 4.x's crop on the
 * iPhone 4 is linear at the stride it programs (432 pixels for a 427-wide
 * crop, not a multiple of 64), which reading the source as packed at
 * ALIGN(w, 64) sheared into streaks.
 * ponytail: nearest sampling where the hardware has polyphase taps at +0x70
 * on.
 */
/* An RGB transfer's work in two halves: the source rows read in, then the destination rows turned and written. */
typedef struct ScalerRgbJob {
    IPodScalerState *s;
    const uint32_t *r;
    uint32_t *src, *dst;
    const size_t *xo, *yo;
    unsigned sw, sh, dw, dh, ss, ds, pitch;
    bool stiled, dtiled, rot, cw, turn4, direct, ok[2];
    const struct ScalerRow *srows, *drows;
} ScalerRgbJob;

/* A row of a surface in host memory: its bytes before `split` from lo, the rest from hi (both indexed from the row's
 * start), since a row in guest RAM can run over into a page that is somewhere else on the host. */
typedef struct ScalerRow {
    uint8_t *lo, *hi;
    unsigned split;
} ScalerRow;

#ifdef __aarch64__
static inline void *scaler_at(const ScalerRow *row, unsigned off)
{
    return (off < row->split ? row->lo : row->hi) + off;
}

/* The host address of the guest RAM page at bus address `va` and its RAM offset; NULL if it is unmapped or not RAM. */
static uint8_t *scaler_ram_page(IPodScalerState *s, uint32_t va, bool write, ram_addr_t *ram)
{
    hwaddr pa = scaler_pa(s, va), xlat, len = 0x1000;
    MemoryRegion *mr;

    if (pa == (hwaddr)-1 || (pa & 0xfff)) {
        return NULL;
    }
    mr = address_space_translate(&address_space_memory, pa, &xlat, &len, write, MEMTXATTRS_UNSPECIFIED);
    if (!memory_region_is_ram(mr) || (write && mr->readonly) || len < 0x1000) {
        return NULL;
    }
    *ram = memory_region_get_ram_addr(mr) + xlat;
    return qemu_map_ram_ptr(NULL, *ram);
}

/*
 * Rows 0..n of a surface (as scaler_row addresses them) as host pointers into guest RAM, each page translated once
 * while the rows walk it; a written surface's pages go into `pages` to be marked dirty. False if a page is not RAM,
 * a row is longer than a page, or a row does not start on 16 bytes (the turn's 4-pixel groups would straddle pages).
 * The caller holds the RCU read lock while the pointers are in use.
 */
static bool scaler_rows_ram(IPodScalerState *s, ScalerRow *out, unsigned n, uint32_t base, unsigned stride,
                            unsigned bytes, bool tiled, GArray *pages)
{
    uint32_t seen[2] = { 1, 1 };   /* never a page address */
    uint8_t *host[2] = { NULL, NULL };
    unsigned next = 0;

    if (bytes > 0x1000) {
        return false;
    }
    for (unsigned y = 0; y < n; y++) {
        uint32_t va = scaler_row(base, y, stride, ROUND_UP(bytes, 256), tiled);
        uint32_t pg[2] = { va & ~0xfffu, (va + bytes - 1) & ~0xfffu };
        uint8_t *p[2];

        if (va & 15) {
            return false;
        }
        for (int k = 0; k < 2; k++) {
            if (k && pg[1] == pg[0]) {
                p[1] = p[0];
                break;
            }
            if (seen[0] == pg[k] || seen[1] == pg[k]) {
                p[k] = host[seen[1] == pg[k]];
                continue;
            }
            ram_addr_t ram;
            p[k] = scaler_ram_page(s, pg[k], pages != NULL, &ram);
            if (!p[k]) {
                return false;
            }
            if (pages) {
                g_array_append_val(pages, ram);
            }
            seen[next] = pg[k];
            host[next] = p[k];
            next ^= 1;
        }
        out[y].lo = p[0] + (va & 0xfff);
        out[y].split = 0x1000 - (va & 0xfff);
        out[y].hi = p[1] - out[y].split;
    }
    return true;
}
#endif

#ifdef __aarch64__
/* d0..d3 get the 4x4 block whose rows are s0..s3, turned about its diagonal. */
static inline void scaler_t4(const uint32_t *s0, const uint32_t *s1, const uint32_t *s2, const uint32_t *s3,
                             uint32_t *d0, uint32_t *d1, uint32_t *d2, uint32_t *d3)
{
    uint32x4_t a = vld1q_u32(s0), b = vld1q_u32(s1), c = vld1q_u32(s2), d = vld1q_u32(s3);
    uint64x2_t ab0 = vreinterpretq_u64_u32(vtrn1q_u32(a, b)), ab1 = vreinterpretq_u64_u32(vtrn2q_u32(a, b));
    uint64x2_t cd0 = vreinterpretq_u64_u32(vtrn1q_u32(c, d)), cd1 = vreinterpretq_u64_u32(vtrn2q_u32(c, d));

    vst1q_u32(d0, vreinterpretq_u32_u64(vtrn1q_u64(ab0, cd0)));
    vst1q_u32(d1, vreinterpretq_u32_u64(vtrn1q_u64(ab1, cd1)));
    vst1q_u32(d2, vreinterpretq_u32_u64(vtrn2q_u64(ab0, cd0)));
    vst1q_u32(d3, vreinterpretq_u32_u64(vtrn2q_u64(ab1, cd1)));
}

/*
 * Destination rows lo..hi of a quarter turn at 1:1, in 4x4 blocks without the offset tables: destination (x, y) is
 * source row r(x) = cw ? sh - 1 - x : x, column c(y) = cw ? y : sw - 1 - y. Four destination rows read four
 * adjacent source columns, which turned the other way come out last row first. Rows are found through the job's row
 * tables: the scratch copies, or the surfaces in guest RAM themselves.
 */
static void scaler_turn4(ScalerRgbJob *j, unsigned lo, unsigned hi)
{
    unsigned sw = j->sw, sh = j->sh, dw = j->dw;
    int step = j->cw ? -1 : 1;

    for (unsigned bx = 0; bx < dw; bx += 32) {
        for (unsigned y = lo; y < hi; y += 4) {
            unsigned c = (j->cw ? y : sw - 4 - y) * 4;
            const ScalerRow *d0 = &j->drows[j->cw ? y : y + 3], *d1 = &j->drows[j->cw ? y + 1 : y + 2];
            const ScalerRow *d2 = &j->drows[j->cw ? y + 2 : y + 1], *d3 = &j->drows[j->cw ? y + 3 : y];
            for (unsigned x = bx; x < bx + 32 && x < dw; x += 4) {
                const ScalerRow *r = &j->srows[j->cw ? sh - 1 - x : x];
                scaler_t4(scaler_at(r, c), scaler_at(r + step, c), scaler_at(r + 2 * step, c),
                          scaler_at(r + 3 * step, c), scaler_at(d0, x * 4), scaler_at(d1, x * 4),
                          scaler_at(d2, x * 4), scaler_at(d3, x * 4));
            }
        }
    }
}
#endif

static void scaler_rgb_part(ScalerRgbJob *j, int phase, unsigned half)
{
    if (phase == 0) {
        unsigned lo = half ? j->sh / 2 : 0, hi = half ? j->sh : j->sh / 2;

        if (j->direct) {   /* turned straight from guest RAM */
            return;
        }

        for (unsigned y = lo; y < hi; y++) {
            if (!scaler_read_bus(j->s, scaler_row(j->r[0x14 / 4], y, j->ss, ROUND_UP(j->sw * 4, 256), j->stiled),
                                 j->src + (size_t)y * j->pitch, j->sw * 4)) {
                j->ok[half] = false;
                return;
            }
        }
        return;
    }
    /* Destination rows in 32-row bands, so each half keeps the turned copy's 32x32 blocks. */
    unsigned mid = MIN(ROUND_UP(j->dh / 2, 32), j->dh);
    unsigned lo = half ? mid : 0, hi = half ? j->dh : mid;
    const uint32_t *src = j->src;
    uint32_t *dst = j->dst;

#ifdef __aarch64__
    if (j->turn4) {
        /* A band at a time, written out while it is still in the cache (or turned into guest RAM directly). */
        for (unsigned by = lo; by < hi; by += 32) {
            unsigned be = MIN(by + 32, hi);
            scaler_turn4(j, by, be);
            for (unsigned y = by; y < be && !j->direct; y++) {
                if (!scaler_write_bus(j->s, scaler_row(j->r[0x34 / 4], y, j->ds, ROUND_UP(j->dw * 4, 256), j->dtiled),
                                      dst + (size_t)y * j->dw, j->dw * 4)) {
                    j->ok[half] = false;
                    return;
                }
            }
        }
        return;
    }
#endif
    if (j->rot) {
        for (unsigned by = lo; by < hi; by += 32) {
            for (unsigned bx = 0; bx < j->dw; bx += 32) {
                for (unsigned x = bx; x < MIN(bx + 32, j->dw); x++) {
                    const uint32_t *col = src + j->xo[x];
                    for (unsigned y = by; y < MIN(by + 32, hi); y++) {
                        dst[(size_t)y * j->dw + x] = col[j->yo[y]];
                    }
                }
            }
        }
    } else {
        for (unsigned y = lo; y < hi; y++) {
            const uint32_t *line = src + j->yo[y];
            uint32_t *row = dst + (size_t)y * j->dw;
            for (unsigned x = 0; x < j->dw; x++) {
                row[x] = line[j->xo[x]];
            }
        }
    }
    for (unsigned y = lo; y < hi; y++) {
        if (!scaler_write_bus(j->s, scaler_row(j->r[0x34 / 4], y, j->ds, ROUND_UP(j->dw * 4, 256), j->dtiled),
                              dst + (size_t)y * j->dw, j->dw * 4)) {
            j->ok[half] = false;
            return;
        }
    }
}

static void *scaler_helper(void *opaque)
{
    IPodScalerState *s = opaque;

    rcu_register_thread();
    qemu_mutex_lock(&s->lock);
    for (;;) {
        while (s->helper_seen == s->helper_gen) {
            qemu_cond_wait(&s->helper_go, &s->lock);
        }
        s->helper_seen = s->helper_gen;
        ScalerRgbJob *j = s->helper_job;
        int phase = s->helper_phase;
        qemu_mutex_unlock(&s->lock);
        scaler_rgb_part(j, phase, 1);
        qemu_mutex_lock(&s->lock);
        s->helper_finished = s->helper_seen;
        qemu_cond_broadcast(&s->helper_done);
    }
    return NULL;
}

/*
 * Both halves of a phase, the second on the helper thread when the worker runs a large transfer
 * (LT_SCALER_SPLIT=off: one thread). CoreAnimation turns every frame of a landscape game, 640x960 on
 * the iPod touch 4G: ~3.3 ms of one Pixel 10 mid core, often still running at the VBL that frame
 * was for.
 */
static void scaler_split(IPodScalerState *s, ScalerRgbJob *j, int phase)
{
    static int split = -1;

    if (split < 0) {
        const char *v = getenv("LT_SCALER_SPLIT");
        split = !v || strcmp(v, "off");
    }
    if (!split || !s->on_worker || (size_t)j->dw * j->dh < 65536) {
        scaler_rgb_part(j, phase, 0);
        scaler_rgb_part(j, phase, 1);
        return;
    }
    if (!s->helper_up) {
        qemu_cond_init(&s->helper_go);
        qemu_cond_init(&s->helper_done);
        qemu_thread_create(&s->helper, "scaler-2", scaler_helper, s, QEMU_THREAD_DETACHED);
        s->helper_up = true;
    }
    qemu_mutex_lock(&s->lock);
    s->helper_job = j;
    s->helper_phase = phase;
    s->helper_gen++;
    qemu_cond_signal(&s->helper_go);
    qemu_mutex_unlock(&s->lock);
    scaler_rgb_part(j, phase, 0);
    qemu_mutex_lock(&s->lock);
    while (s->helper_finished != s->helper_gen) {
        qemu_cond_wait(&s->helper_done, &s->lock);
    }
    qemu_mutex_unlock(&s->lock);
}

static bool scaler_rgb(IPodScalerState *s)
{
    uint32_t *r = s->job;
    unsigned sw = (r[0x24 / 4] >> 16) & 0x1fff, sh = r[0x24 / 4] & 0x1fff;
    unsigned dw = (r[0x40 / 4] >> 16) & 0x1fff, dh = r[0x40 / 4] & 0x1fff;
    unsigned ss = (r[0x1c / 4] & 0xffff) * 4, ds = (r[0x3c / 4] & 0xffff) * 4, pitch = sw;
    uint32_t sx = r[0x50 / 4], sy = r[0x54 / 4];
    bool stiled = r[0x10 / 4] & 0x400, dtiled = r[0x30 / 4] & 0x400;

    gles_present_wait();   /* the source may be a GL frame still being copied in */
    if ((r[0x10 / 4] & 7) != 6 || (r[0x30 / 4] & 7) != 6 || !sw || !sh ||
        !dw || !dh || sw > 2048 || sh > 2048 || dw > 2048 || dh > 2048 ||
        ss < sw * 4 || ds < dw * 4) {
        return false;
    }
    /* Rotated 90 degrees when the steps walk the destination's rows down the source's
     * columns: a 1x iPhone app on the iPad's landscape panel (320x480 -> 480x320 at 1:1,
     * 960x640 at 2x), the way the rest of the portrait UI is turned. */
    bool rot = sx && sy && sw != sh && ((uint64_t)dw * sx) >> 16 == sh && ((uint64_t)dh * sy) >> 16 == sw;
    /* +0x58 bits 0-1 are the turn (11D257 AppleM2ScalerCSCDriver 0x808457f8): 3 is a quarter turn clockwise,
     * the source's top row along the destination's right edge. CoreAnimation asks for it to put a landscape-right
     * app's 960x640 layer on the iPhone 4's portrait panel; turned the other way, the app showed upside down
     * (issue 47). */
    bool cw = rot && (r[0x58 / 4] & 3) == 3;
    sx = sx ? sx : ((uint64_t)sw << 16) / dw;
    sy = sy ? sy : ((uint64_t)sh << 16) / dh;
    uint32_t *src = scaler_scratch(s, 0, sizeof(uint32_t) * pitch * sh);
    /*
     * Destination (x, y) is src[xo[x] + yo[y]]. Turned, a destination row walks a source column, so the
     * pixels go in 32x32 blocks: the whole frame turned a pixel at a time missed the cache on every read,
     * and CoreAnimation turns every frame of a landscape game this way (Jetpack Joyride on the iPod touch 4G).
     */
    g_autofree size_t *xo = g_new(size_t, dw), *yo = g_new(size_t, dh);
    for (unsigned x = 0; x < dw; x++) {
        unsigned i = MIN(((uint64_t)x * sx) >> 16, (rot ? sh : sw) - 1);
        xo[x] = cw ? (size_t)(sh - 1 - i) * pitch : rot ? (size_t)i * pitch : i;
    }
    for (unsigned y = 0; y < dh; y++) {
        unsigned i = MIN(((uint64_t)y * sy) >> 16, (rot ? sw : sh) - 1);
        yo[y] = cw ? i : rot ? sw - 1 - i : (size_t)i * pitch;
    }
    uint32_t *dst = scaler_scratch(s, 1, sizeof(uint32_t) * dw * dh);
    /*
     * A quarter turn at 1:1 (a landscape game on the iPod touch 4G, every frame) goes through scaler_turn4 on arm64
     * hosts: about a third less time than the offset tables on a Poco F3 mid core (LT_SCALER_TURN4=off for A/B).
     */
    static int turn4 = -1;
    if (turn4 < 0) {
        const char *v = getenv("LT_SCALER_TURN4");
        turn4 = !v || strcmp(v, "off");
    }
    ScalerRgbJob j = {
        .s = s, .r = r, .src = src, .dst = dst, .xo = xo, .yo = yo, .sw = sw, .sh = sh, .dw = dw, .dh = dh,
        .ss = ss, .ds = ds, .pitch = pitch, .stiled = stiled, .dtiled = dtiled, .rot = rot, .cw = cw,
        .turn4 = turn4 && rot && sx == 0x10000 && sy == 0x10000 && dw == sh && dh == sw && !(dw % 4) && !(dh % 4),
        .ok = { true, true },
    };
#ifdef __aarch64__
    /*
     * The turn reads and writes the surfaces in guest RAM when both are there (LT_SCALER_DIRECT=off for A/B): no copy
     * of the source in, none of the result out through the bus a page at a time. The written pages are marked dirty
     * as a bus write would; their translated code, which a frame's pages never hold, is left alone.
     */
    static int direct = -1;
    if (direct < 0) {
        const char *v = getenv("LT_SCALER_DIRECT");
        direct = !v || strcmp(v, "off");
    }
    g_autofree ScalerRow *srows = NULL, *drows = NULL;
    g_autoptr(GArray) pages = NULL;
    RCU_READ_LOCK_GUARD();
    if (j.turn4) {
        srows = g_new(ScalerRow, sh);
        drows = g_new(ScalerRow, dh);
        pages = g_array_new(false, false, sizeof(ram_addr_t));
        j.direct = direct && scaler_rows_ram(s, srows, sh, r[0x14 / 4], ss, sw * 4, stiled, NULL) &&
                   scaler_rows_ram(s, drows, dh, r[0x34 / 4], ds, dw * 4, dtiled, pages);
        if (!j.direct) {
            for (unsigned y = 0; y < sh; y++) {
                srows[y] = (ScalerRow){ (uint8_t *)(src + (size_t)y * pitch), NULL, UINT_MAX };
            }
            for (unsigned y = 0; y < dh; y++) {
                drows[y] = (ScalerRow){ (uint8_t *)(dst + (size_t)y * dw), NULL, UINT_MAX };
            }
        }
        j.srows = srows;
        j.drows = drows;
    }
#endif

    scaler_split(s, &j, 0);
    if (!j.ok[0] || !j.ok[1]) {
        return false;
    }
    scaler_split(s, &j, 1);
#ifdef __aarch64__
    for (guint i = 0; j.direct && i < pages->len; i++) {
        physical_memory_set_dirty_range(g_array_index(pages, ram_addr_t, i), 0x1000, DIRTY_CLIENTS_NOCODE);
    }
#endif
    return j.ok[0] && j.ok[1];
}

/*
 * NV12 to NV12, turned 90 degrees when the destination's orientation is the source's turned: how 4.x puts a
 * decoded movie frame (VXD's 480x270 at stride 512) upright for the iPhone 4's portrait panel
 * (270x480 at stride 320), formats +0x10/+0x30 both 0, planes at +0x14/+0x18 and +0x34/+0x38, plane
 * strides in bytes at +0x1c and +0x3c (UV << 16 | Y). The camera's viewfinder takes the same path scaled
 * down: the ISP's 2592x1936 frame to 640x852, turned. Each side's planes are tiled or linear as its format's
 * bit 10 says (scaler_row), as for RGB: AppleM2ScalerCSCHal sets the bit whatever the format.
 * ponytail: nearest sampling where the hardware has polyphase taps, and one turning direction (the movie
 * came out upright with it); +0x58 (3 here) likely selects it.
 */
static bool scaler_nv12(IPodScalerState *s)
{
    uint32_t *r = s->job;
    unsigned sw = (r[0x24 / 4] >> 16) & 0x1fff, sh = r[0x24 / 4] & 0x1fff;
    unsigned dw = (r[0x40 / 4] >> 16) & 0x1fff, dh = r[0x40 / 4] & 0x1fff;
    unsigned sys = r[0x1c / 4] & 0xffff, suvs = r[0x1c / 4] >> 16;
    unsigned dys = r[0x3c / 4] & 0xffff, duvs = r[0x3c / 4] >> 16;
    bool rot = sw != sh && dw != dh && (sw > sh) != (dw > dh);
    bool stiled = r[0x10 / 4] & 0x400, dtiled = r[0x30 / 4] & 0x400;

    if ((r[0x10 / 4] & 7) || (r[0x30 / 4] & 7) || !sw || !sh || sw > 4096 || sh > 4096 || (sw | sh) & 1 ||
        !dw || !dh || (dw | dh) & 1 || sys < sw || suvs < sw || dys < dw || duvs < dw) {
        return false;
    }
    uint8_t *y = scaler_scratch(s, 0, (size_t)sw * sh), *uv = scaler_scratch(s, 1, (size_t)sw * sh / 2);
    g_autofree uint8_t *row = g_malloc(dw);
    for (unsigned i = 0; i < sh; i++) {
        if (!scaler_read_bus(s, scaler_row(r[0x14 / 4], i, sys, ROUND_UP(sw, 256), stiled), y + (size_t)i * sw, sw) ||
            (!(i & 1) && !scaler_read_bus(s, scaler_row(r[0x18 / 4], i / 2, suvs, ROUND_UP(sw, 256), stiled),
                                          uv + (size_t)i / 2 * sw, sw))) {
            return false;
        }
    }
    /*
     * Destination (x, j) samples source row sh - 1 - x, column j when turned, scaled to the source's size:
     * y[xo[x] + yo[j]]. The offsets come from tables, not two 64-bit divides a pixel (every movie frame).
     */
    g_autofree size_t *xo = g_new(size_t, dw), *yo = g_new(size_t, dh);
    for (unsigned x = 0; x < dw; x++) {
        xo[x] = rot ? (size_t)(sh - 1 - (uint64_t)x * sh / dw) * sw : (uint64_t)x * sw / dw;
    }
    for (unsigned j = 0; j < dh; j++) {
        yo[j] = rot ? (uint64_t)j * sw / dh : (size_t)((uint64_t)j * sh / dh) * sw;
    }
    for (unsigned j = 0; j < dh; j++) {
        const uint8_t *line = y + yo[j];
        for (unsigned x = 0; x < dw; x++) {
            row[x] = line[xo[x]];
        }
        if (!scaler_write_bus(s, scaler_row(r[0x34 / 4], j, dys, ROUND_UP(dw, 256), dtiled), row, dw)) {
            return false;
        }
    }
    for (unsigned x = 0; x < dw / 2; x++) {      /* chroma pairs, the same turn at half size */
        xo[x] = rot ? (size_t)(sh / 2 - 1 - (uint64_t)x * sh / dw) * sw : 2 * ((uint64_t)x * sw / dw);
    }
    for (unsigned j = 0; j < dh / 2; j++) {
        yo[j] = rot ? 2 * ((uint64_t)j * sw / dh) : (size_t)((uint64_t)j * sh / dh) * sw;
    }
    for (unsigned j = 0; j < dh / 2; j++) {
        const uint8_t *line = uv + yo[j];
        for (unsigned x = 0; x < dw / 2; x++) {
            row[2 * x] = line[xo[x]];
            row[2 * x + 1] = line[xo[x] + 1];
        }
        if (!scaler_write_bus(s, scaler_row(r[0x38 / 4], j, duvs, ROUND_UP(dw, 256), dtiled), row, dw)) {
            return false;
        }
    }
    return true;
}

static bool scaler_range(uint64_t base, unsigned stride, unsigned rows,
                         unsigned bytes)
{
    return rows && stride >= bytes && base >= 0x08000000 &&
           base + (uint64_t)(rows - 1) * stride + bytes <= 0x10000000;
}

static bool scaler_convert(IPodScalerState *s)
{
    uint32_t *r = s->job;
    unsigned w = (r[0x24 / 4] >> 16) & 0x1fff, h = r[0x24 / 4] & 0x1fff;
    unsigned dw = (r[0x40 / 4] >> 16) & 0x1fff, dh = r[0x40 / 4] & 0x1fff;
    unsigned ys = r[0x1c / 4] & 0xffff, uvs = r[0x1c / 4] >> 16;
    unsigned fmt = r[0x30 / 4] & 7, bpp = fmt == 4 ? 2 : 4;
    unsigned ds = (r[0x3c / 4] & 0xffff) * bpp;
    uint32_t ybase = r[0x14 / 4], uvbase = r[0x18 / 4], dest = r[0x34 / 4];
    bool stiled = r[0x10 / 4] & 0x400, dtiled = r[0x30 / 4] & 0x400;
    /* Match the existing RGB/NV12 nearest-sample scaling approximation.
     * Both planes retain their independent strides and tiling. Native Photos
     * uses this path to fit decoded camera images to the screen. */
    if (!w || !h || w > 4096 || h > 4096 || (w | h) & 1 ||
        !dw || !dh || dw > 4096 || dh > 4096 ||
        (r[0x10 / 4] & ~0x400u) || (fmt != 4 && fmt != 6) || (r[0x30 / 4] & ~0x407u) ||
        r[0x20 / 4] || ((r[0x58 / 4] & 3) && (w != dw || h != dh)) || ys < w || uvs < w || ds < dw * bpp ||
        (!s->xlate && (!scaler_range(ybase, ys, h, w) ||
                       !scaler_range(uvbase, uvs, h / 2, w) ||
                       !scaler_range(dest, ds, dh, dw * bpp)))) return false;
    uint8_t *yplane = scaler_scratch(s, 0, (size_t)w * h);
    uint8_t *uvplane = scaler_scratch(s, 1, (size_t)w * h / 2);
    g_autofree uint8_t *row = g_malloc(dw * bpp);
    /* Snapshot sources before writing: IOSurface transfers may alias. Through the DART on the S5L8920 (3GS). */
    for (unsigned y = 0; y < h; y++) {
        if (!scaler_read_bus(s, scaler_row(ybase, y, ys, ROUND_UP(w, 256), stiled), yplane + y * w, w) ||
            (!(y & 1) && !scaler_read_bus(s, scaler_row(uvbase, y / 2, uvs, ROUND_UP(w, 256), stiled),
                                          uvplane + (y / 2) * w, w))) {
            return false;
        }
    }
    int matrix[9];
    for (unsigned i = 0; i < 9; i++) {
        unsigned v = r[0x220 / 4 + i] & 0xfff;
        matrix[i] = (v ^ 0x800) - 0x800;
    }
    /* A scaled turn (+0x58 bits 0-1, which scaler_rgb honors) is refused above, as every scaled transfer was
     * before scaling came in; a 1:1 one is converted unturned, as the movie path always did. An unscaled side
     * steps by one whatever +0x50/+0x54 hold. */
    uint32_t sx = w == dw ? 0x10000 : r[0x50 / 4] ? r[0x50 / 4] : ((uint64_t)w << 16) / dw;
    uint32_t sy = h == dh ? 0x10000 : r[0x54 / 4] ? r[0x54 / 4] : ((uint64_t)h << 16) / dh;
    for (unsigned y = 0; y < dh; y++) {
        unsigned v = MIN(((uint64_t)y * sy) >> 16, h - 1);
        for (unsigned x = 0; x < dw; x++) {
            unsigned u = MIN(((uint64_t)x * sx) >> 16, w - 1);
            int in[] = { yplane[v * w + u] - ((r[1] & 0x200) ? 16 : 0),
                         uvplane[(v / 2) * w + (u & ~1u)] - 128,
                         uvplane[(v / 2) * w + (u & ~1u) + 1] - 128 };
            unsigned rgb[3];
            for (unsigned c = 0; c < 3; c++) {
                int v = (matrix[c * 3] * in[0] + matrix[c * 3 + 1] * in[1] +
                         matrix[c * 3 + 2] * in[2] + 256) >> 9;
                rgb[c] = MIN(255, MAX(0, v));
            }
            if (fmt == 4) {
                stw_le_p(row + x * 2, ((rgb[0] >> 3) << 11) |
                         ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
            } else {
                row[x * 4] = rgb[2]; row[x * 4 + 1] = rgb[1];
                row[x * 4 + 2] = rgb[0]; row[x * 4 + 3] = 255;
            }
        }
        if (!scaler_write_bus(s, scaler_row(dest, y, ds, ROUND_UP(dw * bpp, 256), dtiled), row, dw * bpp)) {
            return false;
        }
    }
    return true;
}

static bool scaler_run(IPodScalerState *s)
{
    return scaler_convert(s) || scaler_rgb(s) || scaler_nv12(s);
}

static void scaler_unsupported(IPodScalerState *s)
{
    error_report("scaler: unsupported or invalid transfer %08x -> %08x geometry %08x -> %08x",
                 s->job[4], s->job[12], s->job[9], s->job[16]);
}

static void *scaler_worker(void *opaque)
{
    IPodScalerState *s = opaque;

    rcu_register_thread();
    qemu_mutex_lock(&s->lock);
    for (;;) {
        while (!s->queued) {
            qemu_cond_wait(&s->cond, &s->lock);
        }
        s->queued = false;
        s->busy = true;
        s->on_worker = true;
        qemu_mutex_unlock(&s->lock);
        if (!scaler_run(s)) {
            scaler_unsupported(s);
        }
        qemu_mutex_lock(&s->lock);
        s->busy = false;
        qemu_cond_broadcast(&s->cond);
    }
    return NULL;
}

static bool scaler_in_flight(IPodScalerState *s)
{
    return qatomic_read(&s->queued) || qatomic_read(&s->busy);
}

/* Until the worker has written the last transfer's pixels (a reset, a new start, a snapshot). */
static void scaler_wait(IPodScalerState *s)
{
    if (!s->worker_up) {
        return;
    }
    qemu_mutex_lock(&s->lock);
    while (s->queued || s->busy) {
        qemu_cond_wait(&s->cond, &s->lock);
    }
    qemu_mutex_unlock(&s->lock);
}

static void scaler_irq(IPodScalerState *s)
{
    qemu_set_irq(s->irq, (s->regs[2] & s->regs[3] & 1) != 0);
}

static uint64_t scaler_read(void *opaque, hwaddr off, unsigned size)
{
    IPodScalerState *s = opaque;
    return s->regs[off / 4];
}

/*
 * A transfer ends some time after its start, as the block's does: 1 ns a destination pixel (0.6 ms for 640x960;
 * LT_SCALER_PS_PER_PIXEL changes it), at least 20 us, and not before the worker has written the pixels. The busy bit
 * (+0x4 bit 0) stays set and the done interrupt waits for the end. The end gates the frame: backboardd swaps the
 * turned frame only once it is done, so a transfer still running at the VBL costs the game a whole frame. At 5 ns a
 * pixel (3 ms) that was a quarter of Jetpack Joyride's frames on the Poco. Done at the start write, the interrupt reached the driver's workloop while the
 * submitting thread held the command gate, and the workloop took the CPU the moment the gate opened: the completion
 * of an asynchronous transfer freed the request before AppleM2ScalerCSCDriver's submit path read the request's
 * callback, so the submit path took the freed memory for a synchronous request, released a null DMA command and
 * the kernel panicked (fault_addr 0x280, in SpringBoard, an iPhone 4 on 4.2.1 putting its third app away).
 */
static void scaler_complete(void *opaque)
{
    IPodScalerState *s = opaque;

    if (scaler_in_flight(s)) {          /* the worker is behind the modelled end: look again shortly */
        timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
        return;
    }

    s->regs[1] &= ~1u;
    s->regs[3] |= 1;
    scaler_irq(s);
}

static void scaler_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IPodScalerState *s = opaque;
    trace_ipod_scaler_reg_write(s->iomem.addr, off, value);
    if (off == 0xc) s->regs[3] &= ~value;
    else s->regs[off / 4] = value;
    if (off == 4 && (value & 2)) {
        scaler_wait(s);
        timer_del(s->done);
        memset(s->regs, 0, sizeof(s->regs));
        s->regs[0x260 / 4] = s->version;
    } else if (off == 4 && (value & 1)) {
        uint64_t pixels = (uint64_t)((s->regs[0x40 / 4] >> 16) & 0x1fff) * (s->regs[0x40 / 4] & 0x1fff);

        scaler_wait(s);
        if (timer_pending(s->done)) {   /* started again before the last one ended: that one ends now */
            timer_del(s->done);
            s->regs[3] |= 1;
        }
        /*
         * The pixel work only touches guest RAM, so it runs beside the vCPU: turning CoreAnimation's every
         * landscape frame was ~10% of the vCPU's time in Jetpack Joyride on the iPod touch 4G.
         */
        static int async = -1;
        if (async < 0) {
            const char *v = getenv("LT_SCALER_ASYNC");
            async = !v || strcmp(v, "off");
        }
        if (!async) {                   /* A/B: in place, without the BQL */
            bool unlock = bql_locked();

            memcpy(s->job, s->regs, sizeof(s->job));
            if (unlock) {
                bql_unlock();
            }
            if (!scaler_run(s)) {
                scaler_unsupported(s);
            }
            if (unlock) {
                bql_lock();
            }
        } else {
            if (!s->worker_up) {
                qemu_thread_create(&s->worker, "scaler", scaler_worker, s, QEMU_THREAD_DETACHED);
                s->worker_up = true;
            }
            qemu_mutex_lock(&s->lock);
            memcpy(s->job, s->regs, sizeof(s->job));
            s->queued = true;
            qemu_cond_signal(&s->cond);
            qemu_mutex_unlock(&s->lock);
        }
        static int ps_per_pixel = -1;
        if (ps_per_pixel < 0) {
            const char *v = getenv("LT_SCALER_PS_PER_PIXEL");
            ps_per_pixel = v ? atoi(v) : 1000;
        }
        timer_mod(s->done, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MAX(20000, pixels * ps_per_pixel / 1000));
    }
    scaler_irq(s);
}

static const MemoryRegionOps scaler_ops = {
    .read = scaler_read, .write = scaler_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* 5.x's reset polls +0x4 and +0x10 with byte loads (9A334 0x809cfddc ldrb): a word access serves them */
    .valid.min_access_size = 1, .valid.max_access_size = 4,
    .impl.min_access_size = 4, .impl.max_access_size = 4,
};

static void scaler_reset(DeviceState *dev)
{
    IPodScalerState *s = (IPodScalerState *)dev;
    scaler_wait(s);
    timer_del(s->done);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x260 / 4] = s->version;
    scaler_irq(s);
}

static void scaler_init(Object *obj)
{
    IPodScalerState *s = (IPodScalerState *)obj;
    memory_region_init_io(&s->iomem, obj, &scaler_ops, s, "scaler-csc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->done = timer_new_ns(QEMU_CLOCK_VIRTUAL, scaler_complete, s);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->cond);
}

static int scaler_pre_save(void *opaque)
{
    scaler_wait(opaque);
    return 0;
}

static int scaler_post_load(void *opaque, int version_id)
{
    scaler_irq(opaque);
    return 0;
}

static const VMStateDescription scaler_vmstate = {
    .name = "ipod-scaler", .version_id = 2, .minimum_version_id = 2,
    .pre_save = scaler_pre_save,
    .post_load = scaler_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodScalerState, 0x1000 / 4),
        VMSTATE_TIMER_PTR(done, IPodScalerState),
        VMSTATE_END_OF_LIST()
    },
};

static void scaler_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &scaler_vmstate;
    device_class_set_legacy_reset(dc, scaler_reset);
}

static const TypeInfo scaler_type = {
    .name = "ipodtouch.scaler", .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodScalerState), .instance_init = scaler_init,
    .class_init = scaler_class_init,
};
static void scaler_register(void) { type_register_static(&scaler_type); }
type_init(scaler_register)
