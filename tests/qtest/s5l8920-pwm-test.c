/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The iPhones' vibration motor on the S5L8920X PWM (hw/arm/s5l8920_pwm.c): it
 * runs while its channel's control has the run bit, as AppleS5L8920XPWM starts
 * it for a buzz (0x4207) and stops it (0), and the other channels leave it be.
 * The 3GS wires channel 0, the iPhone 4 channel 1 (each DT's pwm/vibrator reg). The iPhone 4's motor also
 * runs from its driver's enable GPIO 0x0e07, the way 6.x and 7.x drive it (pmu/vib-pwm's function-enable).
 * The iPhone 5 (S5L8950X) has the SoC PWM at 0x33500000 (channel 1, its DT's pwm/vibrator) and the driver's enable
 * GPIO 0x0b07; 14G61 holds the enable for a whole pattern and runs the channel for each buzz, so the motor runs only
 * while both are on.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define PWM_BASE    0x83500000
#define PWM_CTRL(n) (PWM_BASE + 0x18 + 4 * (n))
#define A6_PWM_CTRL(n) (0x33500000 + 0x18 + 4 * (n))
#define A4_GPIO_BASE 0xbfa00000
#define A6_GPIO_BASE 0x3fa00000
#define GPIO_CFG(base, dt) ((base) + 4 * (((dt) >> 8) * 8 + ((dt) & 0xff)))
#define GPIO_OUT_LOW  0x2
#define GPIO_OUT_HIGH 0x3

static char *kboot, *a6_kboot;

static bool vibrating(QTestState *q)
{
    QDict *r = qtest_qmp(q, "{ 'execute': 'qom-get', 'arguments': { 'path': '/machine', 'property': 'vibrator' } }");
    bool v = qdict_get_bool(r, "return");

    qobject_unref(r);
    return v;
}

static void check(const char *machine, int channel, uint16_t enable)
{
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                machine, kboot);

    g_assert_false(vibrating(q));
    for (int n = 0; n < 3; n++) {
        if (n == channel) {
            continue;
        }
        qtest_writel(q, PWM_CTRL(n), 0x4003);
        g_assert_false(vibrating(q));
        qtest_writel(q, PWM_CTRL(n), 0);
    }
    /* A buzz as the driver writes it: the two counts, then the control. */
    qtest_writel(q, PWM_BASE + 8 * channel, 0xb386e0ab);
    qtest_writel(q, PWM_BASE + 8 * channel + 4, 0);
    g_assert_false(vibrating(q));
    qtest_writel(q, PWM_CTRL(channel), 0x4207);
    g_assert_true(vibrating(q));
    g_assert_cmphex(qtest_readl(q, PWM_CTRL(channel)), ==, 0x4207);
    qtest_writel(q, PWM_CTRL(channel), 0);
    g_assert_false(vibrating(q));
    if (enable) {
        qtest_writel(q, GPIO_CFG(A4_GPIO_BASE, enable), GPIO_OUT_HIGH);
        g_assert_true(vibrating(q));
        qtest_writel(q, GPIO_CFG(A4_GPIO_BASE, enable), GPIO_OUT_LOW);
        g_assert_false(vibrating(q));
    }
    /* A machine reset stops it, from the PWM or from the enable. */
    if (enable) {
        qtest_writel(q, GPIO_CFG(A4_GPIO_BASE, enable), GPIO_OUT_HIGH);
        qtest_system_reset(q);
        g_assert_false(vibrating(q));
    }
    qtest_writel(q, PWM_CTRL(channel), 0x4207);
    qtest_system_reset(q);
    g_assert_false(vibrating(q));
    qtest_quit(q);
}

static void n88(void) { check("n88", 0, 0); }
static void n90(void) { check("iPhone-4", 1, 0x0e07); }

/* The iPhone 5: the motor runs while the enable and PWM channel 1 both are on; its neighbours don't run it, and a
 * reset stops it. */
static void n42(void)
{
    QTestState *q = qtest_initf("-machine iPhone-5,kboot=%s -display none -audio driver=none -nic none", a6_kboot);

    g_assert_false(vibrating(q));
    qtest_writel(q, GPIO_CFG(A6_GPIO_BASE, 0x0b06), GPIO_OUT_HIGH);
    qtest_writel(q, GPIO_CFG(A6_GPIO_BASE, 0x0c00), GPIO_OUT_HIGH);
    g_assert_false(vibrating(q));
    qtest_writel(q, GPIO_CFG(A6_GPIO_BASE, 0x0b07), GPIO_OUT_HIGH);
    g_assert_false(vibrating(q));
    qtest_writel(q, A6_PWM_CTRL(0), 0x4003);
    qtest_writel(q, A6_PWM_CTRL(2), 0x4003);
    g_assert_false(vibrating(q));
    /* A pattern: the enable held, the channel run and stopped for each buzz. */
    for (int buzz = 0; buzz < 2; buzz++) {
        qtest_writel(q, A6_PWM_CTRL(1), 0x4003);
        g_assert_true(vibrating(q));
        qtest_writel(q, A6_PWM_CTRL(1), 0x4000);
        g_assert_false(vibrating(q));
    }
    qtest_writel(q, A6_PWM_CTRL(1), 0x4003);
    qtest_writel(q, GPIO_CFG(A6_GPIO_BASE, 0x0b07), GPIO_OUT_LOW);
    g_assert_false(vibrating(q));
    qtest_writel(q, GPIO_CFG(A6_GPIO_BASE, 0x0b07), GPIO_OUT_HIGH);
    g_assert_true(vibrating(q));
    qtest_system_reset(q);
    g_assert_false(vibrating(q));
    qtest_quit(q);
}

static char *kboot_file(uint32_t base)
{
    /* A K48KBOOT bundle with an empty image: the trailer only (load, entry, arguments, length). */
    uint8_t trailer[24] = { 'K', '4', '8', 'K', 'B', 'O', 'O', 'T' };
    char *path;
    int fd;

    for (int i = 0; i < 3; i++) {
        stl_le_p(trailer + 8 + 4 * i, base);
    }
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &path, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    return path;
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    kboot = kboot_file(0x40000000);
    a6_kboot = kboot_file(0x80000000);
    qtest_add_func("/s5l8920/pwm/n88-vibrator", n88);
    qtest_add_func("/s5l8920/pwm/n90-vibrator", n90);
    qtest_add_func("/s5l8920/pwm/n42-vibrator", n42);
    ret = g_test_run();
    unlink(kboot);
    unlink(a6_kboot);
    g_free(kboot);
    g_free(a6_kboot);
    return ret;
}
