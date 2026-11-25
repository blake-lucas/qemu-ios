#include "hw/arm/ipod_touch_lis302dl.h"
#include "migration/vmstate.h"
#include "hw/arm/ipod_touch_lcd.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/ipod-attitude.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"
#include "hw/trace-printf.h"

/*
 * The host-facing vector (attitude, QOM x/y/z, shake) is in 1/64 g. That is
 * the LIS331DLH's high byte at ±2 g (1 mg/digit, 12 bits left-justified), so it
 * passes through unchanged. The LIS302DL is 8-bit at 18 mg/digit (±2 g, FS=0)
 * or 72 mg/digit (±8 g, FS=1): 1 g reads 55.6 or 13.9 counts. 3.1.3's driver
 * converts with 18 mg (1187/65536 g per count), 1.0's with 1/128 g, so a part
 * lying flat reads -1.00 g on 3.x and -0.43 g on 1.0, as on a real device.
 * A state with no part identity (whoami 0, unit tests) passes through too.
 */

static int lis302dl_counts(const LIS302DLState *s, int v)
{
    if (s->whoami != ACCEL_WHOAMI_VALUE) {
        return v;
    }
    return lround(v * 1000.0 / (64 * (s->ctrl_reg1 & ACCEL_CTRL_REG1_FS ? 72 : 18)));
}

static bool lis302dl_debug(void)
{
    return trace_event_get_state_backends(TRACE_LIS302DL_LOG);
}

/* The mounted sensor's reading of a device vector g (in g), in g. */
static void lis302dl_mount(LIS302DLState *s, const double g[3], double out[3])
{
    if (s->dt_orientation && !s->axis[0]) {
        if (!ios_dt_mount(s->dt_orientation, true, s->axis)) {
            s->axis[0] = 0;
        } else if (s->flip_z) {
            for (int i = 0; i < 3; i++) {
                s->axis[i] = abs(s->axis[i]) == 3 ? -s->axis[i] : s->axis[i];
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        double v = g[i];
        if (s->axis[0]) {
            int a = (s->axis[i] < 0 ? -s->axis[i] : s->axis[i]) - 1;
            v = s->axis[i] < 0 ? -g[a] : g[a];
        }
        out[i] = v;
    }
}

/* The mounted sensor's reading of a device vector g (in g), in 1/64 g. */
static void lis302dl_mounted(LIS302DLState *s, const double g[3], int out[3])
{
    double m[3];

    lis302dl_mount(s, g, m);
    for (int i = 0; i < 3; i++) {
        out[i] = lround(64 * m[i]);
    }
}

/*
 * With motion set (a board whose gyro sees the turn), a new attitude is not a
 * jump: the device turns to it about one axis at 90 degrees per 350 ms, the
 * accelerometer reads the gravity of each moment, and lis302dl_motion_rate
 * gives the gyro the turn's rate. CoreMotion's sensor fusion trusts the gyro
 * over the accelerometer: fed a jump with no rotation, it took about 7 s to
 * follow a landscape flip and came round through the screen's axis.
 */
#define LIS_MOTION_NS_PER_RAD (350000000.0 / (M_PI / 2))

static bool lis302dl_moving(LIS302DLState *s, int64_t now)
{
    return s->motion_start_ns >= 0 && now >= s->motion_start_ns &&
           now < s->motion_start_ns + s->motion_ns;
}

/* The device's gravity vector (in g) at `now`: along the turn while moving. */
static void lis302dl_gravity(LIS302DLState *s, int64_t now, double g[3])
{
    if (!lis302dl_moving(s, now)) {
        ipod_attitude_gravity(s->pitch_mdeg / 1000.0, s->roll_mdeg / 1000.0, s->flat_pose, g);
        return;
    }
    const double *v = s->motion_from, *k = s->motion_axis;
    double t = s->motion_angle * (now - s->motion_start_ns) / s->motion_ns;
    double c = cos(t), sn = sin(t), kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    double kxv[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };

    for (int i = 0; i < 3; i++) {
        g[i] = v[i] * c + kxv[i] * sn + k[i] * kv * (1 - c);
    }
}

bool lis302dl_motion_rate(LIS302DLState *s, int64_t now, double w[3])
{
    if ((s->host_fields & 2) && now < s->host_gyro_until_ns) {
        memcpy(w, s->host_angular_velocity, sizeof(s->host_angular_velocity));
        return true;
    }
    bool moving = lis302dl_moving(s, now);
    /* Gravity turns by +angle about k in the device's frame: the device turns by -angle. */
    double r = moving ? -s->motion_angle * 1e9 / s->motion_ns : 0;

    for (int i = 0; i < 3; i++) {
        w[i] = r * s->motion_axis[i];
    }
    return moving;
}

/* Turn from gravity `from` to the current attitude's, about the axis between them. */
static void lis302dl_start_motion(LIS302DLState *s, const double from[3], int64_t now)
{
    double to[3], k[3], n, dot;

    ipod_attitude_gravity(s->pitch_mdeg / 1000.0, s->roll_mdeg / 1000.0, s->flat_pose, to);
    k[0] = from[1] * to[2] - from[2] * to[1];
    k[1] = from[2] * to[0] - from[0] * to[2];
    k[2] = from[0] * to[1] - from[1] * to[0];
    n = sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    dot = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    s->motion_angle = atan2(n, dot);
    s->motion_start_ns = -1;
    if (s->motion_angle < M_PI / 180) {
        return;
    }
    if (n < 1e-6) {
        /* Half a turn: about the screen's axis when upright, else the device's x. */
        double kv;
        k[0] = fabs(from[2]) < 0.7 ? 0 : 1;
        k[1] = 0;
        k[2] = fabs(from[2]) < 0.7 ? 1 : 0;
        kv = k[0] * from[0] + k[2] * from[2];
        for (int i = 0; i < 3; i++) {
            k[i] -= kv * from[i];
        }
        n = sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    }
    for (int i = 0; i < 3; i++) {
        s->motion_axis[i] = k[i] / n;
        s->motion_from[i] = from[i];
    }
    s->motion_ns = s->motion_angle * LIS_MOTION_NS_PER_RAD;
    s->motion_start_ns = now;
}

bool lis302dl_apply_attitude(LIS302DLState *s, double pitch, double roll, bool flat)
{
    double g[3], from[3];
    int v[3];
    int64_t now = s->motion ? qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) : 0;

    if (!ipod_attitude_gravity(pitch, roll, flat, g)) {
        return false;
    }
    if (s->motion) {
        lis302dl_gravity(s, now, from);
    }
    s->pitch_mdeg = lround(pitch * 1000);
    s->roll_mdeg = lround(roll * 1000);
    s->flat_pose = flat;
    s->host_fields &= ~1u;      /* a pose set here replaces the host's held acceleration */
    lis302dl_mounted(s, g, v);
    s->base_x = v[0];
    s->base_y = v[1];
    s->base_z = v[2];
    if (s->motion) {
        lis302dl_start_motion(s, from, now);
    }
    return true;
}

/* UIDeviceOrientation shares the same mounted attitude model. */
void lis302dl_apply_orientation(LIS302DLState *s, uint32_t o)
{
    double roll = 0;
    bool flat = o == 5 || o == 6;
    if (o == 2 || o == 6) roll = 180;
    else if (o == 3) roll = 90;
    else if (o == 4) roll = -90;
    lis302dl_apply_attitude(s, 0, roll, flat);
    s->orientation = o;
    it_display_set_orientation(o);
    if (lis302dl_debug()) {
        TRACE_PRINTF(trace_lis302dl_log, "lis302dl: orientation=%u -> x=%d y=%d z=%d\n",
               o, s->base_x, s->base_y, s->base_z);
    }
}

/* Generate only on a guest sample boundary. Repeated reads within one period
 * see the same triplet, regardless of the host input event rate. Lazy sampling
 * avoids an always-running timer on a part whose interrupt pin is not modeled. */
static void lis302dl_sample(LIS302DLState *s, int64_t now)
{
    uint32_t rate = s->rate_hz ? s->rate_hz : (s->ctrl_reg1 & 0x80 ? 400 : 100);
    int64_t period = 1000000000LL / rate;
    if (s->last_sample_ns >= 0 && now >= s->last_sample_ns &&
        now - s->last_sample_ns < period) return;
    s->last_sample_ns = now;
    int values[3] = { lis302dl_counts(s, s->base_x), lis302dl_counts(s, s->base_y),
                      lis302dl_counts(s, s->base_z) };
    if (lis302dl_moving(s, now)) {
        double g[3];
        lis302dl_gravity(s, now, g);
        lis302dl_mounted(s, g, values);
        for (int i = 0; i < 3; i++) {
            values[i] = lis302dl_counts(s, values[i]);
        }
    }
    if (s->host_fields & 1) {
        double g[3];
        for (int i = 0; i < 3; i++) g[i] = -s->host_acceleration[i] / 9.80665;
        lis302dl_mounted(s, g, values);
        for (int i = 0; i < 3; i++) values[i] = lis302dl_counts(s, values[i]);
    }
    int64_t elapsed = now - s->shake_start_ns;
    if (s->shake_start_ns >= 0 && elapsed >= 0 && elapsed < 200000000) {
        int impulse = lis302dl_counts(s, (elapsed / 20000000) & 1 ? -127 : 127);
        values[0] = impulse;
        values[1] = -impulse;
        values[2] = impulse / 2;
    } else {
        s->shake_start_ns = -1;
        if (values[0] || values[1] || values[2]) {
            for (int i = 0; i < 3; i++) {
                /* Device-local PRNG: migration is reproducible and motion does
                 * not consume guest entropy or depend on host wall time. */
                uint32_t x = s->noise_state ? s->noise_state : 0x302d1;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                s->noise_state = x;
                values[i] += (int)(x % 3) - 1;
            }
        }
    }
    s->out_x = CLAMP(values[0], -128, 127);
    s->out_y = CLAMP(values[1], -128, 127);
    s->out_z = CLAMP(values[2], -128, 127);
}

static void lis302dl_trace_poll(LIS302DLState *s, int64_t now)
{
    if (!trace_event_get_state_backends(TRACE_LIS302DL_POLL_LOG)) {
        return;
    }
    if (s->trace_last_poll_ns >= 0 && now > s->trace_last_poll_ns) {
        s->trace_poll_sum_ns += now - s->trace_last_poll_ns;
        s->trace_polls++;
    }
    s->trace_last_poll_ns = now;
    if (now - s->trace_last_report_ns >= 1000000000LL && s->trace_polls) {
        TRACE_PRINTF(trace_lis302dl_poll_log, "[IT_ACCEL] OUT_X polls=%u mean_interval_ms=%.3f\n",
                s->trace_polls, s->trace_poll_sum_ns / (s->trace_polls * 1000000.0));
        s->trace_last_report_ns = now;
        s->trace_poll_sum_ns = 0;
        s->trace_polls = 0;
    }
}

void lis302dl_shake(LIS302DLState *s)
{
    s->shake_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

void lis302dl_set_axis_value(LIS302DLState *s, char axis, int v)
{
    if (v < -128) v = -128;
    if (v > 127) v = 127;
    switch (axis) {
        case 'x': s->base_x = (int8_t)v; break;
        case 'y': s->base_y = (int8_t)v; break;
        case 'z': s->base_z = (int8_t)v; break;
        default: break;
    }
}

/* ---- LIS3DSH (WHO_AM_I 0x3F): the iPhone 5's "accelerometer,ap3dsh" ----
 *
 * 14G61's AppleAP3DSH probes WHO_AM_I (0x0f) for 0x3f, reads the part's
 * temperature (OUT_T, 0x0c), and runs it at 400 Hz with the FIFO in stream
 * mode: INT1 wakes handleInterrupt1, which reads FIFO_SRC (0x2f) and, with
 * WTM set, reads FSS samples (1-31) in one burst from OUT_X_L (0x28), at most
 * 128 bytes. So the outputs wrap 0x2d -> 0x28 while the FIFO is on, and a
 * slot pops when OUT_Z_H is read, as the gyro model's do (s5l8930_i2c.c).
 * Counts are 16-bit at the full scale CTRL_REG5 selects (datasheet
 * sensitivities, 0.06 mg/digit at +-2 g). The pins are at their level: INT1
 * carries data-ready (CTRL_REG3 DR_EN) and the FIFO's empty/watermark/overrun
 * routes (CTRL_REG6 P1_*), active high with IEA; INT2's state machines are
 * not modelled (the pin rests inactive).
 */
#define DS_OUT_T        0x0c
#define DS_INFO1        0x0d
#define DS_CTRL4        0x20    /* ODR[7:4] BDU Zen Yen Xen */
#define DS_CTRL3        0x23    /* DR_EN IEA IEL INT2_EN INT1_EN VFILT - STRT */
#define DS_CTRL5        0x24    /* BW[7:6] FSCALE[5:3] ST[2:1] SIM */
#define DS_CTRL6        0x25    /* BOOT FIFO_EN WTM_EN ADD_INC P1_EMPTY P1_WTM P1_OVERRUN P2_BOOT */
#define DS_STATUS       0x27
#define DS_OUT_X_L      0x28
#define DS_OUT_Z_H      0x2d
#define DS_FIFO_CTRL    0x2e    /* FMODE[7:5] WTMP[4:0] */
#define DS_FIFO_SRC     0x2f    /* WTM OVRN EMPTY FSS[4:0] */

static bool lis3dsh(const LIS302DLState *s)
{
    return s->whoami == ACCEL_WHOAMI_LIS3DSH;
}

static unsigned lis3dsh_mode(LIS302DLState *s)
{
    return s->ds_regs[DS_CTRL6] & 0x40 ? s->ds_regs[DS_FIFO_CTRL] >> 5 : 0;
}

/* The device's acceleration as the part reads it, in counts at the selected full scale. */
static void lis3dsh_measure(LIS302DLState *s, int64_t now)
{
    static const double mg_per_digit[8] = { 0.06, 0.12, 0.18, 0.24, 0.73, 0.73, 0.73, 0.73 };
    double g[3], m[3], cpg = 1000.0 / mg_per_digit[(s->ds_regs[DS_CTRL5] >> 3) & 7];
    int64_t elapsed = now - s->shake_start_ns;

    if (s->host_fields & 1) {
        for (int i = 0; i < 3; i++) {
            g[i] = -s->host_acceleration[i] / 9.80665;
        }
    } else if (lis302dl_moving(s, now)) {
        lis302dl_gravity(s, now, g);
    } else {
        ipod_attitude_gravity(s->pitch_mdeg / 1000.0, s->roll_mdeg / 1000.0, s->flat_pose, g);
    }
    lis302dl_mount(s, g, m);
    if (s->shake_start_ns >= 0 && elapsed >= 0 && elapsed < 200000000) {
        double impulse = (elapsed / 20000000) & 1 ? -2 : 2;
        m[0] = impulse;
        m[1] = -impulse;
        m[2] = impulse / 2;
    } else {
        s->shake_start_ns = -1;
    }
    for (int i = 0; i < 3; i++) {
        /* a few counts of noise (the part's is ~0.1 mg rms); see lis302dl_sample's PRNG */
        uint32_t x = s->noise_state ? s->noise_state : 0x302d1;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s->noise_state = x;
        s->ds_sample[i] = CLAMP(lround(m[i] * cpg) + (int)(x % 5) - 2, INT16_MIN, INT16_MAX);
    }
}

/*
 * LT_LIS3DSH_WTM_STRICT=off: the watermark flag (and its INT1 route) from WTM samples on, as before. By default it is up
 * only with more than WTM samples. iOS 10 sets WTM 1 and, on INT1, reads FIFO_SRC and drains the FIFO only once it
 * holds two samples: with the flag at one sample, the level interrupt stayed asserted between samples and the AP core
 * took it again and again, ~900-1,500 FIFO_SRC reads a second in Epic Citadel against ~50 drains.
 */
static bool lis3dsh_wtm_strict(void)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("LT_LIS3DSH_WTM_STRICT");

        on = !(v && (!strcmp(v, "off") || !strcmp(v, "0")));
    }
    return on;
}

static uint8_t lis3dsh_fifo_src(LIS302DLState *s)
{
    uint8_t wtm = s->ds_regs[DS_FIFO_CTRL] & 0x1f;
    unsigned need = lis3dsh_wtm_strict() ? wtm + 1u : MAX(wtm, 1);

    /* WTM only with samples to read: AppleAP3DSH reads FSS of them and does nothing for 0, so a flag up on an
     * empty FIFO would hold INT1 asserted (the gyro model's storm). FSS has five bits: a full FIFO reads 31. */
    return (s->ds_level && s->ds_level >= need ? 0x80 : 0) | (s->ds_ovrn ? 0x40 : 0) |
           (s->ds_level ? 0 : 0x20) | MIN(s->ds_level, 0x1f);
}

static void lis3dsh_update(LIS302DLState *s)
{
    uint8_t c3 = s->ds_regs[DS_CTRL3], c6 = s->ds_regs[DS_CTRL6], src = lis3dsh_fifo_src(s);
    bool high = c3 & 0x40;
    bool int1 = (c3 & 0x08) &&
                (((c3 & 0x80) && (s->ds_regs[DS_STATUS] & 0x08)) || ((c6 & 0x04) && (src & 0x80)) ||
                 ((c6 & 0x02) && (src & 0x40)) || ((c6 & 0x08) && (src & 0x20)));

    qemu_set_irq(s->ds_pin[0], int1 == high);
    qemu_set_irq(s->ds_pin[1], !high);
}

/* The sample at the head of the FIFO (or the latest one) into OUT_X..OUT_Z. */
static void lis3dsh_load_out(LIS302DLState *s)
{
    const int16_t *v = lis3dsh_mode(s) && s->ds_level ? s->ds_fifo : s->ds_sample;

    for (int i = 0; i < 3; i++) {
        stw_le_p(&s->ds_regs[DS_OUT_X_L + 2 * i], v[i]);
    }
}

static void lis3dsh_pop(LIS302DLState *s)
{
    if (lis3dsh_mode(s) && s->ds_level) {
        memmove(s->ds_fifo, s->ds_fifo + 3, sizeof(s->ds_sample) * --s->ds_level);
        s->ds_ovrn = false;
    }
    s->ds_regs[DS_STATUS] = 0;
    lis3dsh_load_out(s);
}

static void lis3dsh_schedule(LIS302DLState *s)
{
    /* CTRL_REG4 ODR: power down, 3.125, 6.25, 12.5, 25, 50, 100, 400, 800, 1600 Hz */
    static const int64_t period_ns[16] = {
        0, 320000000, 160000000, 80000000, 40000000, 20000000, 10000000, 2500000, 1250000, 625000,
    };
    int64_t period = period_ns[s->ds_regs[DS_CTRL4] >> 4];

    if (period && s->ds_timer) {
        timer_mod(s->ds_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period);
    } else if (s->ds_timer) {
        timer_del(s->ds_timer);
    }
}

static void lis3dsh_tick(void *opaque)
{
    LIS302DLState *s = opaque;
    unsigned mode = lis3dsh_mode(s);

    lis3dsh_measure(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    if (mode) {
        if (s->ds_level == LIS3DSH_FIFO_SLOTS) {
            s->ds_ovrn = true;
            if (mode != 1) {             /* stream modes drop the oldest; FIFO mode stops */
                memmove(s->ds_fifo, s->ds_fifo + 3, sizeof(s->ds_sample) * --s->ds_level);
            }
        }
        if (s->ds_level < LIS3DSH_FIFO_SLOTS) {
            memcpy(s->ds_fifo + 3 * s->ds_level++, s->ds_sample, sizeof(s->ds_sample));
        }
    }
    s->ds_regs[DS_STATUS] = (s->ds_regs[DS_STATUS] & 0x08 ? 0xf0 : 0) | 0x0f;
    lis3dsh_load_out(s);
    lis3dsh_update(s);
    lis3dsh_schedule(s);
}

static void lis3dsh_reset(LIS302DLState *s)
{
    if (s->ds_timer) {
        timer_del(s->ds_timer);
    }
    memset(s->ds_regs, 0, sizeof(s->ds_regs));
    s->ds_regs[DS_INFO1] = 0x21;
    s->ds_regs[ACCEL_WHOAMI] = ACCEL_WHOAMI_LIS3DSH;
    s->ds_regs[DS_CTRL4] = 0x07;         /* axes enabled, powered down */
    s->ds_regs[DS_CTRL6] = 0x10;         /* ADD_INC */
    s->ds_level = 0;
    s->ds_ovrn = false;
    memset(s->ds_sample, 0, sizeof(s->ds_sample));
}

static uint8_t lis3dsh_recv(LIS302DLState *s)
{
    uint8_t reg = s->cmd & 0x3f, v;
    bool inc = (s->cmd & 0x80) || (s->ds_regs[DS_CTRL6] & 0x10);

    v = reg == DS_FIFO_SRC ? lis3dsh_fifo_src(s) : s->ds_regs[reg];
    if (lis302dl_debug()) {
        TRACE_PRINTF(trace_lis302dl_log, "lis3dsh: read 0x%02x -> 0x%02x\n", reg, v);
    }
    if (inc) {
        s->cmd = (s->cmd & 0x80) | (reg == DS_OUT_Z_H && lis3dsh_mode(s) ? DS_OUT_X_L : (reg + 1) & 0x3f);
    }
    if (reg == DS_OUT_Z_H) {
        lis3dsh_pop(s);
        lis3dsh_update(s);
    }
    return v;
}

static void lis3dsh_send(LIS302DLState *s, uint8_t data)
{
    uint8_t reg = s->cmd & 0x3f;

    if (lis302dl_debug()) {
        TRACE_PRINTF(trace_lis302dl_log, "lis3dsh: write 0x%02x <- 0x%02x\n", reg, data);
    }
    if ((s->cmd & 0x80) || (s->ds_regs[DS_CTRL6] & 0x10)) {
        s->cmd = (s->cmd & 0x80) | ((reg + 1) & 0x3f);
    }
    if ((reg >= 0x10 && reg <= 0x1f) || (reg >= DS_CTRL4 && reg <= DS_CTRL6) || reg == DS_FIFO_CTRL ||
        (reg >= 0x40 && reg <= 0x7f)) {
        s->ds_regs[reg] = data;
    }
    if (reg == DS_CTRL6 && (data & 0x80)) {
        s->ds_regs[DS_CTRL6] &= 0x7f;      /* BOOT reloads trim and clears itself */
    }
    if (reg == DS_FIFO_CTRL || reg == DS_CTRL6) {
        if (!lis3dsh_mode(s)) {             /* bypass empties the FIFO */
            s->ds_level = 0;
            s->ds_ovrn = false;
        }
        lis3dsh_load_out(s);
    }
    lis3dsh_update(s);
    if (reg == DS_CTRL4) {
        lis3dsh_schedule(s);
    }
}

static int lis302dl_event(I2CSlave *i2c, enum i2c_event event)
{
    LIS302DLState *s = LIS302DL(i2c);
    if (event == I2C_START_SEND) {
        /* the first byte of a write is the sub-address / register pointer */
        s->pointer_set = false;
    }
    return 0;
}

static uint8_t lis302dl_recv(I2CSlave *i2c)
{
    LIS302DLState *s = LIS302DL(i2c);
    uint8_t ret;

    if (lis3dsh(s)) {
        return lis3dsh_recv(s);
    }
    /* Bit 7 of the pointer is the auto-increment flag on both parts; the
     * LIS331DLH driver sets it for its 16-bit OUT_X_L/H bursts. */
    uint8_t reg = s->cmd & 0x7f;

    if (reg == ACCEL_OUT_X || reg == ACCEL_OUT_Y || reg == ACCEL_OUT_Z) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        lis302dl_sample(s, now);
        if (reg == ACCEL_OUT_X) lis302dl_trace_poll(s, now);
    }
    switch(reg) {
        case ACCEL_WHOAMI:
            ret = s->whoami;
            break;
        case ACCEL_STATUS:
            /* All axes have fresh data available (ZYXDA + per-axis DA), no
             * overrun. A driver that polls STATUS for data-ready before
             * reading OUT_X/Y/Z needs this to be non-zero. */
            ret = 0x0F;
            break;
        /*
         * The iOS AppleLIS302DL driver only sets the PD (power, 0x40) bit in
         * CTRL_REG1 and then polls OUT_X/Y/Z -- it never sets the per-axis
         * enable bits, and CTRL_REG1 even reads back 0 between poll cycles.
         * Gating the outputs on per-axis (or even PD) bits therefore starves
         * the OS of samples and rotation never happens. Report the injected
         * acceleration whenever the sensor is not held in explicit power-down,
         * i.e. as long as CTRL_REG1 has ever been programmed non-zero.
         */
        /*
         * LIS331DLH: 12-bit left-justified 16-bit outputs, OUT_x_L at 0x28/
         * 0x2A/0x2C and OUT_x_H at the LIS302DL's 8-bit addresses. A zero low
         * byte makes the high byte the same 64-counts/g value; AppleLIS331DLH
         * then does (v + 15) >> 4 and applies the DT orientation matrix.
         */
        case ACCEL_OUT_X - 1:
        case ACCEL_OUT_Y - 1:
        case ACCEL_OUT_Z - 1:
            ret = 0;
            break;
        case ACCEL_OUT_X:
            ret = (uint8_t)s->out_x;
            break;
        case ACCEL_OUT_Y:
            ret = (uint8_t)s->out_y;
            break;
        case ACCEL_OUT_Z:
            ret = (uint8_t)s->out_z;
            break;
        case ACCEL_CTRL_REG1:
            ret = s->ctrl_reg1;
            break;
        case ACCEL_CTRL_REG2:
            ret = s->ctrl_reg2;
            break;
        case ACCEL_CTRL_REG3:
            ret = s->ctrl_reg3;
            break;
        default:
            if (lis302dl_debug()) {
                TRACE_PRINTF(trace_lis302dl_log, "%s: unknown register 0x%02x\n", __func__, reg);
            }
            ret = 0;
            break;
    }
    if (lis302dl_debug()) {
        TRACE_PRINTF(trace_lis302dl_log, "lis302dl: read reg 0x%02x -> 0x%02x\n", reg, ret);
    }
    /* honor the multi-byte / auto-increment read the driver uses to slurp
     * OUT_X/Y/Z in one burst */
    s->cmd++;
    return ret;
}

static int lis302dl_send(I2CSlave *i2c, uint8_t data)
{
    LIS302DLState *s = LIS302DL(i2c);

    if (!s->pointer_set) {
        s->cmd = data;
        s->pointer_set = true;
        return 0;
    }
    if (lis3dsh(s)) {
        lis3dsh_send(s, data);
        return 0;
    }

    /* a data byte written to the current register */
    switch (s->cmd & 0x7f) {
        case ACCEL_CTRL_REG1: s->ctrl_reg1 = data; break;
        case ACCEL_CTRL_REG2:
            /*
             * BOOT (bit 6) reloads the part's trimming registers and the
             * hardware clears it when that finishes. AppleLIS302DL sets it in
             * enableAccelerometer and polls for it to return to zero, and
             * panics the kernel outright if it has not cleared within 500 ms:
             *   "AppleLIS302DL::enableAccelerometer - Boot bit did not return
             *    to zero in 500 msecs. That is wrong."
             * Echoing the write back kept the bit set forever, so 3.1.3 panicked
             * as soon as SpringBoard brought the accelerometer up. Complete the
             * reboot immediately and clear the bit. The LIS331DLH (WHO_AM_I
             * 0x32, the iPad) keeps BOOT in bit 7 instead; 4.x's AppleLIS331DLH
             * polls it the same way and panics likewise.
             */
            s->ctrl_reg2 = data & ~(s->whoami == ACCEL_WHOAMI_LIS331DLH ?
                                    ACCEL_CTRL_REG2_BOOT_LIS331 : ACCEL_CTRL_REG2_BOOT);
            break;
        case ACCEL_CTRL_REG3: s->ctrl_reg3 = data; break;
        default:
            if (lis302dl_debug()) {
                TRACE_PRINTF(trace_lis302dl_log, "%s: write 0x%02x to reg 0x%02x\n", __func__, data, s->cmd);
            }
            break;
    }
    s->cmd++;
    return 0;
}

/* --- QMP-drivable orientation / shake, exposed as QOM properties --- */

static void lis302dl_get_orientation(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val = s->orientation;
    visit_type_int(v, name, &val, errp);
}

static void lis302dl_set_orientation(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    lis302dl_apply_orientation(s, (uint32_t)val);
}

static void lis302dl_get_axis(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    /* QOM reports the requested raw vector; I2C reports sampled sensor data. */
    int8_t *axis = (name[0] == 'x') ? &s->base_x : (name[0] == 'y') ? &s->base_y : &s->base_z;
    int64_t val = *axis;
    visit_type_int(v, name, &val, errp);
}

static void lis302dl_set_axis(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    int64_t val;
    if (!visit_type_int(v, name, &val, errp)) {
        return;
    }
    lis302dl_set_axis_value(s, name[0], (int)CLAMP(val, -128, 127));
}

static void lis302dl_set_shake(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    LIS302DLState *s = LIS302DL(obj);
    bool val;
    if (!visit_type_bool(v, name, &val, errp)) {
        return;
    }
    if (val) {
        lis302dl_shake(s);
    }
}

static void lis302dl_init(Object *obj)
{
    LIS302DLState *s = LIS302DL(obj);

    /* Power-on default: upright in portrait. */
    lis302dl_apply_orientation(s, 1);
    s->last_sample_ns = s->shake_start_ns = s->trace_last_poll_ns = -1;
    s->motion_start_ns = -1;
    s->noise_state = 0x302d1;
    s->out_x = s->base_x; s->out_y = s->base_y; s->out_z = s->base_z;

    object_property_add(obj, "orientation", "int",
                        lis302dl_get_orientation, lis302dl_set_orientation, NULL, NULL);
    object_property_add(obj, "x", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "y", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "z", "int", lis302dl_get_axis, lis302dl_set_axis, NULL, NULL);
    object_property_add(obj, "shake", "bool", NULL, lis302dl_set_shake, NULL, NULL);
}

static int lis302dl_post_load(void *opaque, int version)
{
    LIS302DLState *s = opaque;
    lis302dl_clear_host_motion(s);
    if (version < 2) {
        s->pitch_mdeg = 0;
        s->flat_pose = s->orientation == 5 || s->orientation == 6;
        s->roll_mdeg = s->orientation == 2 || s->orientation == 6 ? 180000 :
                      s->orientation == 3 ? 90000 : s->orientation == 4 ? -90000 : 0;
    }
    if (s->pitch_mdeg < -180000 || s->pitch_mdeg > 180000 ||
        s->roll_mdeg < -180000 || s->roll_mdeg > 180000) return -EINVAL;
    if (version < 3) { s->rate_hz = 0; s->noise_state = 0x302d1; }
    if (s->rate_hz > 400) return -EINVAL;
    s->last_sample_ns = s->shake_start_ns = s->trace_last_poll_ns = -1;
    s->motion_start_ns = -1;              /* a turn in progress lands where it was going */
    s->trace_last_report_ns = s->trace_poll_sum_ns = s->trace_polls = 0;
    s->out_x = s->base_x;
    s->out_y = s->base_y;
    s->out_z = s->base_z;
    return 0;
}

static bool lis3dsh_needed(void *opaque)
{
    return lis3dsh(opaque);
}

static int lis3dsh_post_load(void *opaque, int version_id)
{
    LIS302DLState *s = opaque;

    if (s->ds_level > LIS3DSH_FIFO_SLOTS) {
        return -EINVAL;                     /* lis3dsh_pop moves ds_level slots */
    }
    lis3dsh_schedule(s);
    return 0;
}

/* The LIS3DSH's own registers and FIFO, only where the part is one (other boards' states are unchanged). */
static const VMStateDescription vmstate_lis3dsh = {
    .name = "lis302dl/lis3dsh",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = lis3dsh_needed,
    .post_load = lis3dsh_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ds_regs, LIS302DLState, 0x40),
        VMSTATE_UINT8(ds_level, LIS302DLState),
        VMSTATE_BOOL(ds_ovrn, LIS302DLState),
        VMSTATE_INT16_ARRAY(ds_fifo, LIS302DLState, LIS3DSH_FIFO_SLOTS * 3),
        VMSTATE_INT16_ARRAY(ds_sample, LIS302DLState, 3),
        VMSTATE_END_OF_LIST()
    }
};

/* Shake is transient host input, not retained after restore: a restore
 * lands with the accelerometer at rest at its steady-state vector. */
static const VMStateDescription vmstate_lis302dl = {
    .name = "lis302dl",
    .version_id = 3,
    .minimum_version_id = 1,
    .post_load = lis302dl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, LIS302DLState),
        VMSTATE_UINT32(cmd, LIS302DLState),
        VMSTATE_BOOL(pointer_set, LIS302DLState),
        VMSTATE_INT8(out_x, LIS302DLState),
        VMSTATE_INT8(out_y, LIS302DLState),
        VMSTATE_INT8(out_z, LIS302DLState),
        VMSTATE_INT8(base_x, LIS302DLState),
        VMSTATE_INT8(base_y, LIS302DLState),
        VMSTATE_INT8(base_z, LIS302DLState),
        VMSTATE_UINT32(orientation, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg1, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg2, LIS302DLState),
        VMSTATE_UINT16(ctrl_reg3, LIS302DLState),
        VMSTATE_INT32_V(pitch_mdeg, LIS302DLState, 2),
        VMSTATE_INT32_V(roll_mdeg, LIS302DLState, 2),
        VMSTATE_BOOL_V(flat_pose, LIS302DLState, 2),
        VMSTATE_UINT32_V(rate_hz, LIS302DLState, 3),
        VMSTATE_UINT32_V(noise_state, LIS302DLState, 3),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_lis3dsh,
        NULL
    }
};

static void lis302dl_realize(DeviceState *dev, Error **errp)
{
    LIS302DLState *s = LIS302DL(dev);

    /* gpio-out 0/1: INT1/INT2, driven by the LIS3DSH only */
    qdev_init_gpio_out(dev, s->ds_pin, 2);
    if (lis3dsh(s)) {
        s->ds_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lis3dsh_tick, s);
        lis3dsh_reset(s);
    }
}

static void lis302dl_reset(DeviceState *dev)
{
    LIS302DLState *s = LIS302DL(dev);

    if (lis3dsh(s)) {
        lis3dsh_reset(s);
        lis3dsh_update(s);
    }
}

static const Property lis302dl_properties[] = {
    DEFINE_PROP_UINT8("whoami", LIS302DLState, whoami, ACCEL_WHOAMI_VALUE),
    DEFINE_PROP_STRING("dt-orientation", LIS302DLState, dt_orientation),
    DEFINE_PROP_BOOL("flip-z", LIS302DLState, flip_z, false),
};

static void lis302dl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_lis302dl;
    dc->realize = lis302dl_realize;
    device_class_set_legacy_reset(dc, lis302dl_reset);
    device_class_set_props(dc, lis302dl_properties);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = lis302dl_event;
    k->recv = lis302dl_recv;
    k->send = lis302dl_send;
}

static const TypeInfo lis302dl_info = {
    .name          = TYPE_LIS302DL,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = lis302dl_init,
    .instance_size = sizeof(LIS302DLState),
    .class_init    = lis302dl_class_init,
};

static void lis302dl_register_types(void)
{
    type_register_static(&lis302dl_info);
}

type_init(lis302dl_register_types)

/*
 * Host samples, in device portrait axes; the sensor mount and CTRL full-scale
 * conversion still make the guest's registers. The last acceleration holds
 * until the host lets go (lis302dl_clear_host_motion) or sets a pose
 * (lis302dl_apply_attitude), as a device held still keeps reading the same
 * gravity: a host sends when its own sensor or the player moves, and a gap
 * between samples is not the device turning. It used to lapse 250 ms after a
 * sample to the board's resting pose (upright portrait on the iPod touch 2G),
 * so a host that paused - a controller held steady, a UI frame late - read
 * in the guest as the device snapping upright and back, which tilt-steered
 * games (Real Racing) took as the steering recentring. A rotation rate is
 * motion, not a pose: it still lapses to zero, the device then at rest where
 * the last acceleration put it.
 */
void lis302dl_clear_host_motion(LIS302DLState *s)
{
    s->host_fields = 0;
    s->host_gyro_until_ns = 0;
    memset(s->host_acceleration, 0, sizeof(s->host_acceleration));
    memset(s->host_angular_velocity, 0, sizeof(s->host_angular_velocity));
}
void lis302dl_set_host_motion(LIS302DLState *s, uint32_t fields, const double a[3], const double w[3])
{
    if (fields & 1) memcpy(s->host_acceleration, a, sizeof(s->host_acceleration));
    if (fields & 2) {
        memcpy(s->host_angular_velocity, w, sizeof(s->host_angular_velocity));
        s->host_gyro_until_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 250000000;
    }
    s->host_fields |= fields;
}
