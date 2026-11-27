/*
 * Intersil ISL29003 ambient light sensor on I2C1 (0x44), its INT on GPIO 0x25.
 *
 * Eight byte registers behind a pointer: the first byte of a write transfer
 * sets it, and reads and further writes run on from it. While enabled and not
 * powered down, the ADC converts continuously; each conversion (2^width cycles
 * of an oscillator set by the external resistor) latches the count for the
 * `lux` property and raises INT_FLAG after PERSIST conversions outside the
 * threshold window, which only software clears. Adapted from Martijn de Vos's
 * ISL29003 model (devos50/qemu-ios skeuo), with the datasheet's CONTROL layout
 * (range in bits 3:2, persistence in bits 1:0) and full-scale range, which
 * shrinks as the resistor grows.
 */
#include "hw/arm/ipod_touch_isl29003dl.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "trace.h"
#include "hw/trace-printf.h"

#define LIGHT_TRACE(...) do { \
    if (trace_event_get_state_backends(TRACE_ISL29003DL_LOG)) { \
        TRACE_PRINTF(trace_isl29003dl_log, __VA_ARGS__); } } while (0)

/* Oscillator at 100 kOhm; it runs slower as the resistor grows. */
#define ISL29003_OSC_HZ_100K 327680ULL

static unsigned isl29003dl_width_bits(const ISL29003DLState *s)
{
    return 16 - 4 * (s->regs[LIGHTSENSOR_COMMAND] & LIGHTSENSOR_CMD_WIDTH_MASK);
}

static bool isl29003dl_running(const ISL29003DLState *s)
{
    uint8_t cmd = s->regs[LIGHTSENSOR_COMMAND];
    return (cmd & LIGHTSENSOR_CMD_ENABLE) && !(cmd & LIGHTSENSOR_CMD_ADC_PD);
}

/* One conversion: 2^width oscillator cycles. */
static int64_t isl29003dl_conversion_ns(const ISL29003DLState *s)
{
    /* ns = 2^width * 1e9 / (OSC * 100k / rext); 1e9 / 100k folded to keep 64 bits */
    return (int64_t)(((1ULL << isl29003dl_width_bits(s)) * s->rext * 10000ULL) / ISL29003_OSC_HZ_100K);
}

/* Diode 1's count for `lux`: full scale is the range's lux * 100 kOhm / rext. */
static uint16_t isl29003dl_count(const ISL29003DLState *s)
{
    static const uint32_t range_lux[4] = { 1000, 4000, 16000, 64000 };
    unsigned bits = isl29003dl_width_bits(s);
    unsigned range = (s->regs[LIGHTSENSOR_CONTROL] >> LIGHTSENSOR_CTRL_RANGE_SHIFT) & 3;
    uint64_t count = ((uint64_t)s->lux << bits) * s->rext / (range_lux[range] * 100000ULL);

    return MIN(count, (1u << bits) - 1);
}

static void isl29003dl_update_irq(ISL29003DLState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[LIGHTSENSOR_CONTROL] & LIGHTSENSOR_CTRL_INT_FLAG));
}

static void isl29003dl_update_timer(ISL29003DLState *s)
{
    if (!isl29003dl_running(s)) {
        timer_del(s->timer);
        s->out_count = 0;
    } else if (!timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + isl29003dl_conversion_ns(s));
    }
}

/* A conversion completes: latch the selected diode's count, check the window. */
static void isl29003dl_convert(ISL29003DLState *s)
{
    static const uint8_t persist[4] = { 1, 4, 8, 16 };
    unsigned mode = (s->regs[LIGHTSENSOR_COMMAND] >> LIGHTSENSOR_CMD_MODE_SHIFT) & 3;
    /* ponytail: diode 2 (infrared) reads 0, so D1 - D2 = D1; add an IR share if a guest's curves need one */
    uint16_t data = mode == 1 ? 0 : isl29003dl_count(s);
    uint8_t msb = data >> 8;

    s->regs[LIGHTSENSOR_LSB_SENSOR] = data & 0xff;
    s->regs[LIGHTSENSOR_MSB_SENSOR] = data >> 8;
    /* The thresholds are compared with the result's upper byte. */
    if (msb > s->regs[LIGHTSENSOR_IT_HI] || msb < s->regs[LIGHTSENSOR_IT_LO]) {
        if (s->out_count < 16) {
            s->out_count++;
        }
        if (s->out_count >= persist[s->regs[LIGHTSENSOR_CONTROL] & LIGHTSENSOR_CTRL_PERSIST]) {
            s->regs[LIGHTSENSOR_CONTROL] |= LIGHTSENSOR_CTRL_INT_FLAG;
        }
    } else {
        s->out_count = 0;
    }
    isl29003dl_update_irq(s);
}

static void isl29003dl_conversion_done(void *opaque)
{
    ISL29003DLState *s = opaque;

    isl29003dl_convert(s);
    isl29003dl_update_timer(s);
}

static void isl29003dl_write_reg(ISL29003DLState *s, uint8_t reg, uint8_t val)
{
    LIGHT_TRACE("isl29003: write reg %u <- 0x%02x", reg, val);
    switch (reg) {
    case LIGHTSENSOR_COMMAND:
        if ((s->regs[reg] ^ val) & ~(LIGHTSENSOR_CMD_ENABLE | LIGHTSENSOR_CMD_ADC_PD)) {
            timer_del(s->timer);   /* a new width or mode restarts the conversion */
        }
        s->regs[reg] = val;
        isl29003dl_update_timer(s);
        break;
    case LIGHTSENSOR_CONTROL:
        /* INT_FLAG can only be cleared by software, never set. */
        s->regs[reg] = (val & ~LIGHTSENSOR_CTRL_INT_FLAG) |
                       (val & s->regs[reg] & LIGHTSENSOR_CTRL_INT_FLAG);
        isl29003dl_update_irq(s);
        break;
    case LIGHTSENSOR_IT_HI:
    case LIGHTSENSOR_IT_LO:
        s->regs[reg] = val;
        break;
    default:
        break;   /* the data and timer registers are read-only */
    }
}

static int isl29003dl_event(I2CSlave *i2c, enum i2c_event event)
{
    ISL29003DLState *s = ISL29003DL(i2c);

    if (event == I2C_START_SEND) {
        s->reg_written = false;   /* the transfer's first byte is the register */
    }
    return 0;
}

static uint8_t isl29003dl_recv(I2CSlave *i2c)
{
    ISL29003DLState *s = ISL29003DL(i2c);
    uint8_t val = s->regs[s->reg];

    LIGHT_TRACE("isl29003: read reg %u -> 0x%02x", s->reg, val);
    s->reg = (s->reg + 1) % LIGHTSENSOR_NUM_REGS;
    return val;
}

static int isl29003dl_send(I2CSlave *i2c, uint8_t data)
{
    ISL29003DLState *s = ISL29003DL(i2c);

    if (!s->reg_written) {
        /* Only the low address bits are decoded (Martijn de Vos saw the kernel clear INT_FLAG through 0x41). */
        s->reg = data % LIGHTSENSOR_NUM_REGS;
        s->reg_written = true;
        return 0;
    }
    isl29003dl_write_reg(s, s->reg, data);
    s->reg = (s->reg + 1) % LIGHTSENSOR_NUM_REGS;
    return 0;
}

static void isl29003dl_reset(DeviceState *dev)
{
    ISL29003DLState *s = ISL29003DL(dev);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->reg = 0;
    s->reg_written = false;
    s->out_count = 0;
    isl29003dl_update_irq(s);
}

static void isl29003dl_init(Object *obj)
{
    ISL29003DLState *s = ISL29003DL(obj);

    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, isl29003dl_conversion_done, s);
    s->lux = 300;   /* indoor light; qom-set lux at runtime */
    object_property_add_uint32_ptr(obj, "lux", &s->lux, OBJ_PROP_FLAG_READWRITE);
}

static void isl29003dl_finalize(Object *obj)
{
    timer_free(ISL29003DL(obj)->timer);
}

static const Property isl29003dl_properties[] = {
    /* The n72 DT's external-scaling-resistor-ohm. */
    DEFINE_PROP_UINT32("rext", ISL29003DLState, rext, 499000),
};

static int isl29003dl_post_load(void *opaque, int version_id)
{
    ISL29003DLState *s = opaque;
    return s->reg < LIGHTSENSOR_NUM_REGS ? 0 : -EINVAL;   /* reads index regs[] by it */
}

static const VMStateDescription vmstate_isl29003dl = {
    .name = "isl29003dl",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = isl29003dl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, ISL29003DLState),
        VMSTATE_UINT8(reg, ISL29003DLState),
        VMSTATE_BOOL(reg_written, ISL29003DLState),
        VMSTATE_UINT8_ARRAY(regs, ISL29003DLState, LIGHTSENSOR_NUM_REGS),
        VMSTATE_UINT8(out_count, ISL29003DLState),
        VMSTATE_UINT32(lux, ISL29003DLState),
        VMSTATE_TIMER_PTR(timer, ISL29003DLState),
        VMSTATE_END_OF_LIST()
    }
};

static void isl29003dl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_isl29003dl;
    device_class_set_props(dc, isl29003dl_properties);
    device_class_set_legacy_reset(dc, isl29003dl_reset);
    k->event = isl29003dl_event;
    k->recv = isl29003dl_recv;
    k->send = isl29003dl_send;
}

static const TypeInfo isl29003dl_info = {
    .name              = TYPE_ISL29003DL,
    .parent            = TYPE_I2C_SLAVE,
    .instance_init     = isl29003dl_init,
    .instance_finalize = isl29003dl_finalize,
    .instance_size     = sizeof(ISL29003DLState),
    .class_init        = isl29003dl_class_init,
};

static void isl29003dl_register_types(void)
{
    type_register_static(&isl29003dl_info);
}

type_init(isl29003dl_register_types)
