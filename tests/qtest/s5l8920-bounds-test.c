/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Guest-written indices, lengths and counts the S5L8920 and A4 models bound before use (README.md, "Guest values").
 * Each case drives a value past its bound through the registers or memory the guest controls and checks the model
 * refuses it and goes on working: AMC engine 0's command list (a register destination outside the register file,
 * and one that is the list's own head), the VXD's to-host write index and to-MTX write index, the S5L8920 DART's
 * segment index (through the scaler), and the CDMA AES engine's output queue fed past its cap without a drain.
 * No firmware executes.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define SRAM            0x84000000u     /* the AMC's buffer aperture on the A4 */
#define AMC             0x84100000u
#define VXD             0x85000000u
#define VXD_KICK        0x080
#define VXD_TO_HOST_BUF 0x2fe4
#define VXD_TO_HOST_WR  0x2fec
#define VXD_TO_MTX_BUF  0x2ff0
#define VXD_TO_MTX_WR   0x2ffc
#define DART0           0xbfe00000u     /* S5L8920 */
#define SCALER          0x85500000u
#define CDMA            0x87000000u
#define AES             0x87800000u
#define DRAM            0x40000000u

static char *kboot;

static QTestState *start(const char *machine)
{
    return qtest_initf("-machine %s,kboot=%s -display none -audio driver=none -nic none", machine, kboot);
}

/* Kick an engine-0 list of one link (at SRAM + 0x100) that moves one word (at SRAM + 0x200) to AMC register dst. */
static void amc_list(QTestState *q, uint32_t dst, uint32_t word)
{
    uint32_t link[4] = { 0, 4u << 16 | 2u << 2, 0x200, dst };

    qtest_memwrite(q, SRAM + 0x100, link, sizeof(link));
    qtest_writel(q, SRAM + 0x200, word);
    qtest_writel(q, AMC + 0x000, 0x100);                /* AMC_E0_HEAD */
}

static void amc_register_range(void)
{
    QTestState *q = start("ipad1");

    amc_list(q, 0x940, 0x12345678);                     /* in range: the register takes the word */
    g_assert_cmphex(qtest_readl(q, AMC + 0x940), ==, 0x12345678);
    amc_list(q, 0x10000000, 0xdeadbeef);                /* far past the register file */
    amc_list(q, 0x2ffc + 4, 0xdeadbeef);                /* one word past it */
    amc_list(q, 0x940, 0x9abcdef0);                     /* and the engine still works */
    g_assert_cmphex(qtest_readl(q, AMC + 0x940), ==, 0x9abcdef0);
    qtest_quit(q);
}

static void amc_list_head(void)
{
    QTestState *q = start("ipad1");

    amc_list(q, 0x000, 0x100);                          /* the head itself: a list that would run itself */
    amc_list(q, 0x940, 0x9abcdef0);
    g_assert_cmphex(qtest_readl(q, AMC + 0x940), ==, 0x9abcdef0);
    qtest_quit(q);
}

static uint32_t vxd(QTestState *q, uint32_t off) { return qtest_readl(q, VXD + off); }

/* A render message the firmware completes after its setup time, asking for a reply. */
static void vxd_render(QTestState *q, unsigned *w)
{
    uint32_t ring = 0x2000 + (vxd(q, VXD_TO_MTX_BUF) >> 16);
    uint32_t msg[8] = { 0x81 << 8 | 32, 0, 0, 0, 7, 0, 0, 0x4000 };

    for (int i = 0; i < 8; i++) {
        qtest_writel(q, VXD + ring + 4 * (*w + i), msg[i]);
    }
    *w += 8;
    qtest_writel(q, VXD + VXD_TO_MTX_WR, *w);
    qtest_writel(q, VXD + VXD_KICK, 1);
}

static void vxd_to_host_index(void)
{
    QTestState *q = start("ipad1");
    unsigned w = 0;

    qtest_writel(q, VXD + 0x000, 1);                    /* MTX_ENABLE: the comms come up */
    qtest_writel(q, VXD + VXD_TO_HOST_WR, 0x40000000);  /* far past the 0xf0-word ring */
    vxd_render(q, &w);
    qtest_clock_step(q, 60000);
    g_assert_cmpuint(vxd(q, VXD_TO_HOST_WR), <, 0xf0);  /* the reply went into the ring */
    qtest_quit(q);
}

static void vxd_to_mtx_index(void)
{
    QTestState *q = start("ipad1");
    unsigned w = 0;

    qtest_writel(q, VXD + 0x000, 1);
    qtest_writel(q, VXD + VXD_TO_MTX_WR, 0x1000);       /* never meets the read index: the kick must return */
    qtest_writel(q, VXD + VXD_KICK, 1);
    qtest_writel(q, VXD + VXD_TO_MTX_WR, 0);
    vxd_render(q, &w);                                  /* a good message after it still completes */
    qtest_clock_step(q, 60000);
    g_assert_cmpuint(vxd(q, VXD_TO_HOST_WR), ==, 3);
    qtest_quit(q);
}

/* A 2x2 NV12 -> BGRA conversion whose source and destination are past the DART's last segment. */
static void dart_segment(void)
{
    QTestState *q = start("n88");

    qtest_writel(q, DART0 + 0x0c, 1u << 31);            /* DART_CONFIG: translating */
    qtest_writel(q, SCALER + 0x24, 2 << 16 | 2);
    qtest_writel(q, SCALER + 0x40, 2 << 16 | 2);
    qtest_writel(q, SCALER + 0x30, 6);
    qtest_writel(q, SCALER + 0x1c, 16 << 16 | 16);
    qtest_writel(q, SCALER + 0x3c, 16);
    qtest_writel(q, SCALER + 0x14, 0xfffff000);
    qtest_writel(q, SCALER + 0x18, 0xfffff000);
    qtest_writel(q, SCALER + 0x34, 0xfffff000);
    qtest_writel(q, SCALER + 0x04, 1);                  /* start */
    qtest_clock_step(q, 100000);
    g_assert_cmphex(qtest_readl(q, SCALER + 0x0c) & 1, ==, 1);     /* it ended (with nothing translated) */
    qtest_quit(q);
}

/*
 * Channel 1 feeds AES context 1 from memory, five 16 MiB descriptors, and nothing drains the engine's output: past
 * the queue's cap (16 MiB, one segment) the channel stops with an error instead of growing the host's queue.
 */
static void cdma_aes_queue(void)
{
    QTestState *q = start("ipad1");
    uint32_t ctx1 = AES + (1 << 12), ch1 = CDMA + (1 << 12), ring = DRAM + 0x100000;

    qtest_writel(q, ctx1 + 0x00, (1 << 8) | 0x20000 | 0x100000);     /* channel 1, custom 128-bit key, decrypt */
    for (unsigned i = 0; i < 5; i++) {
        uint32_t d[8] = { ring + 32 * (i + 1), (i ? 0x10003 : 0x30003) | (i == 4 ? 0x100 : 0),
                          DRAM + 0x1000000, 16u << 20 };
        qtest_memwrite(q, ring + 32 * i, d, sizeof(d));
    }
    qtest_writel(q, CDMA + 0x00, 1 << 1);
    qtest_writel(q, ch1 + 0x00, 2);
    qtest_writel(q, ch1 + 0x00, 0);
    qtest_writel(q, ch1 + 0x14, ring);
    qtest_writel(q, ch1 + 0x00, 0x188);
    qtest_writel(q, ch1 + 0x0c, 80u << 20);
    qtest_writel(q, ch1 + 0x00, 0x189);                 /* go */
    g_assert_cmphex(qtest_readl(q, ch1 + 0x00) & 0x40000, ==, 0x40000);    /* stopped with an error */
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle with an empty image: the trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    qtest_add_func("/s5l8920/bounds/amc-register-range", amc_register_range);
    qtest_add_func("/s5l8920/bounds/amc-list-head", amc_list_head);
    qtest_add_func("/s5l8920/bounds/vxd-to-host-index", vxd_to_host_index);
    qtest_add_func("/s5l8920/bounds/vxd-to-mtx-index", vxd_to_mtx_index);
    qtest_add_func("/s5l8920/bounds/dart-segment", dart_segment);
    qtest_add_func("/s5l8920/bounds/cdma-aes-queue", cdma_aes_queue);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
