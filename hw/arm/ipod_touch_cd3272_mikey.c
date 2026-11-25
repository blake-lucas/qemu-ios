/*
 * TI CD3272/CD3282 "Mikey" headset controller (i2c0 0x39 on the iPod touch 2G,
 * iPhone 3GS, iPod touch 3G, iPhone 4 and iPad). Without a headset every
 * register reads 0, which is all the drivers need to start.
 *
 * Headphone jack (the "headphones" property, machine option headphones=on;
 * wired on the iPhone 4 and the iPad, whose DTs give the PMU as the interrupt
 * parent). 10B329 (n90) and 9B206 (k48) AppleCD3282Mikey, under
 * AppleEmbeddedAudio's AppleMikeyReceiver, use the part as a MAP register
 * file (bit 7 of the address auto-increments):
 * - 0x0c bit 3 is the jack sense. AppleEmbeddedAudio's HP detect asks the Mikey
 *   (+0x3cc, n90 0x80c3768c) when the codec has no function-hp_detect, as on
 *   both boards; a plugged jack starts headset detection.
 * - Detection is mode 10 (+0x424, 0x80c379dc): 0x00 <- 0xbf, then the masks
 *   0x09 (for status 0x0e) and 0x08 (for status 0x0d), written inverted, with
 *   0x0e bits 3-4 (type detect done) unmasked.
 * - The interrupt handler (+0x448, 0x80c37bf8) reads 0x0d, 0x0e and 0x0a-0x0c
 *   (0x8a, three bytes). With 0x0e bits 3-4 it hands 0x0c bit 0 to
 *   AppleMikeyReceiver's detect result (0x80a44f08), where 1 is HeadphonesOnly:
 *   a plain headphone, no mic or button. AppleEmbeddedAudio then routes to
 *   Headphones, which runs no speaker-protection DSP.
 * ponytail: inferred from the drivers (the 9B206 build has the same registers
 * and modes), not from a datasheet; the status registers are taken as read to
 * clear, as the handler never writes them back. Buttons, mics and plug edges
 * after boot aren't modelled: the jack is plugged from power-on.
 */
#include "hw/arm/ipod_touch_cd3272_mikey.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "hw/trace-printf.h"

#define MIKEY_MAP_INCR      0x80
#define MIKEY_REG_MODE      0x00
#define MIKEY_MODE_DETECT   0x80    /* set only by mode 10, headset detection */
#define MIKEY_REG_MASK_D    0x08    /* mask for 0x0d, 1 = masked */
#define MIKEY_REG_MASK_E    0x09    /* mask for 0x0e, 1 = masked */
#define MIKEY_REG_ACC       0x0c
#define MIKEY_ACC_HP_ONLY   0x01    /* no mic or button */
#define MIKEY_ACC_PLUGGED   0x08
#define MIKEY_REG_STATUS_D  0x0d
#define MIKEY_REG_STATUS_E  0x0e
#define MIKEY_STATUS_TYPE   0x18    /* headset type detect done */

/* -trace cd3272_mikey_log logs every register access; it was unconditional. */
static bool mikey_trace(void)
{
    return trace_event_get_state_backends(TRACE_CD3272_MIKEY_LOG);
}

static void cd3272_mikey_update_irq(CD3272MikeyState *s)
{
    bool level = (s->regs[MIKEY_REG_STATUS_E] & ~s->regs[MIKEY_REG_MASK_E]) ||
                 (s->regs[MIKEY_REG_STATUS_D] & ~s->regs[MIKEY_REG_MASK_D]);

    if (level != s->irq_level) {
        s->irq_level = level;
        qemu_set_irq(s->irq, level);
    }
}

static int cd3272_mikey_event(I2CSlave *i2c, enum i2c_event event)
{
    CD3272MikeyState *s = CD3272MIKEY(i2c);

    if (event == I2C_START_SEND) {
        s->have_cmd = false;    /* the next byte written is the MAP */
    }
    return 0;
}

static uint8_t cd3272_mikey_recv(I2CSlave *i2c)
{
    CD3272MikeyState *s = CD3272MIKEY(i2c);
    uint8_t reg = s->cmd & 0x7f;
    uint8_t res = 0;

    if (s->hp) {
        res = s->regs[reg];
        if (reg == MIKEY_REG_ACC) {
            res |= MIKEY_ACC_PLUGGED | MIKEY_ACC_HP_ONLY;
        } else if (reg == MIKEY_REG_STATUS_D || reg == MIKEY_REG_STATUS_E) {
            s->regs[reg] = 0;
            cd3272_mikey_update_irq(s);
        }
    }
    if (mikey_trace()) {
        TRACE_PRINTF(trace_cd3272_mikey_log, "MIKEY R %02x -> %02x\n", reg, res);
    }
    s->cmd = (reg + 1) & 0x7f;
    return res;
}

static int cd3272_mikey_send(I2CSlave *i2c, uint8_t data)
{
    CD3272MikeyState *s = CD3272MIKEY(i2c);
    uint8_t reg;

    if (!s->have_cmd) {
        s->have_cmd = true;
        s->cmd = data;
        return 0;
    }
    reg = s->cmd & 0x7f;
    if (mikey_trace()) {
        TRACE_PRINTF(trace_cd3272_mikey_log, "MIKEY W %02x <- %02x\n", reg, data);
    }
    if (reg == MIKEY_REG_MODE && s->hp && (data & MIKEY_MODE_DETECT) && !(s->regs[reg] & MIKEY_MODE_DETECT)) {
        /* detection started with a plain headphone in the jack: it's done at once */
        s->regs[MIKEY_REG_STATUS_E] |= MIKEY_STATUS_TYPE;
    }
    s->regs[reg] = data;
    if (s->hp) {
        cd3272_mikey_update_irq(s);
    }
    s->cmd = (reg + 1) & 0x7f;
    return 0;
}

static void cd3272_mikey_reset(DeviceState *dev)
{
    CD3272MikeyState *s = CD3272MIKEY(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MIKEY_REG_MASK_D] = s->regs[MIKEY_REG_MASK_E] = 0xff;
    s->cmd = 0;
    s->have_cmd = false;
    s->irq_level = false;
}

static void cd3272_mikey_init(Object *obj)
{
    qdev_init_gpio_out(DEVICE(obj), &CD3272MIKEY(obj)->irq, 1);
}

static bool cd3272_mikey_hp_needed(void *opaque)
{
    return CD3272MIKEY(opaque)->hp;
}

static const VMStateDescription vmstate_cd3272mikey_hp = {
    .name = "cd3272mikey/hp",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = cd3272_mikey_hp_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(have_cmd, CD3272MikeyState),
        VMSTATE_BOOL(irq_level, CD3272MikeyState),
        VMSTATE_UINT8_ARRAY(regs, CD3272MikeyState, 128),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_cd3272mikey = {
    .name = "cd3272mikey",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(i2c, CD3272MikeyState),
        VMSTATE_UINT32(cmd, CD3272MikeyState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_cd3272mikey_hp,
        NULL
    },
};

static const Property cd3272_mikey_props[] = {
    /* a plain headphone plugged in (see MIKEY_REG_ACC); boards wire the interrupt */
    DEFINE_PROP_BOOL("headphones", CD3272MikeyState, hp, false),
};

static void cd3272_mikey_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_cd3272mikey;
    device_class_set_legacy_reset(dc, cd3272_mikey_reset);
    device_class_set_props(dc, cd3272_mikey_props);
    k->event = cd3272_mikey_event;
    k->recv = cd3272_mikey_recv;
    k->send = cd3272_mikey_send;
}

static const TypeInfo cd3272_mikey_info = {
    .name          = TYPE_CD3272MIKEY,
    .parent        = TYPE_I2C_SLAVE,
    .instance_init = cd3272_mikey_init,
    .instance_size = sizeof(CD3272MikeyState),
    .class_init    = cd3272_mikey_class_init,
};

static void cd3272_mikey_register_types(void)
{
    type_register_static(&cd3272_mikey_info);
}

type_init(cd3272_mikey_register_types)
