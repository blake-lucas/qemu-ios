/*
 * National LM48557 speaker amplifier: the iPod touch 3G's (DT i2c2/spkr-amp, "audio-control,lm48557", I2C 0x6f),
 * fed by the CS42L58's headphone output (audio0's hpout-spkr). iOS sets the speaker's volume here and nowhere else:
 * the PCM stays at full scale and the codec's 0x1a holds one level from 1 to 16, so without this the host heard every
 * game at the same level.
 *
 * Each data byte is a whole register write (TI SNAS486D, tables 3-6): bit 7 clear is Mode Control (bit 1 mute, bit 0
 * normal operation; 0 is shutdown), bit 7 set is Volume Control (bits 5:0, the gain code of table 6). AppleLM48557Amp
 * (8C148) writes both in one transaction, mode then volume: 0x01 0xbf at the slider's top (code 0x3f, +48 dB) down to
 * 0x01 0x8c at level 1 (0x0c, +2 dB), 0x00 0x80 (shutdown) at level 0 and while the device sleeps, 0x03 0x80 then
 * 0x02 0x80 as it powers the speaker path up.
 * ponytail: from a 16 -> 0 -> 8 volume-button trace with Jetpack Joyride playing on the speaker.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "hw/arm/lm48557.h"
#include <math.h>

OBJECT_DECLARE_SIMPLE_TYPE(LM48557State, LM48557)

#define LM48557_VOLUME   0x80   /* bit 7: the Volume Control register, else Mode Control */
#define LM48557_MUTE     0x02
#define LM48557_NORMAL   0x01   /* clear: shutdown */
#define LM48557_TOP_DB   48     /* code 0x3f, iOS's top level: heard at unity */

struct LM48557State {
    I2CSlave parent_obj;
    uint8_t mode, volume;
    bool written;               /* false until iOS sets it: unity, as before this model (and a state saved then) */
};

/* Table 6's gain in dB for each volume code; codes 0 and 0x20 mute (-90 dB). */
static int lm48557_db(unsigned code)
{
    static const int8_t low[8] = { -90, -25, -22, -19, -16, -13, -10, -8 };
    static const int8_t mid[8] = { -6, -4, 0, 4, 8, 12, 14, 16 };   /* codes 0x28-0x2f */

    code &= 0x3f;
    if (code < 0x20) {
        return code < 8 ? low[code] : -6 + 2 * (int)(code - 8);
    }
    if (code < 0x28) {
        return low[code - 0x20];
    }
    if (code < 0x30) {
        return mid[code - 0x28];
    }
    return 18 + 2 * (int)(code - 0x30);   /* 0x30-0x3b: 18-40 dB, 0x3c-0x3f: 42-48 dB */
}

double lm48557_output_gain(void *opaque)
{
    LM48557State *s = LM48557(opaque);
    unsigned code = s->volume & 0x3f;

    if (!s->written) {
        return 1.0;
    }
    if (!(s->mode & LM48557_NORMAL) || (s->mode & LM48557_MUTE) || code == 0 || code == 0x20) {
        return 0.0;
    }
    return MIN(pow(10.0, (lm48557_db(code) - LM48557_TOP_DB) / 20.0), 1.0);
}

static int lm48557_send(I2CSlave *dev, uint8_t data)
{
    LM48557State *s = LM48557(dev);

    if (data & LM48557_VOLUME) {
        s->volume = data;
    } else {
        s->mode = data;
    }
    s->written = true;
    return 0;
}

static uint8_t lm48557_recv(I2CSlave *dev)
{
    return 0xff;   /* write-only */
}

static void lm48557_reset(DeviceState *dev)
{
    LM48557State *s = LM48557(dev);

    s->mode = 0;
    s->volume = LM48557_VOLUME;
    s->written = false;
}

static const VMStateDescription vmstate_lm48557 = {
    .name = TYPE_LM48557,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, LM48557State),
        VMSTATE_UINT8(mode, LM48557State),
        VMSTATE_UINT8(volume, LM48557State),
        VMSTATE_BOOL(written, LM48557State),
        VMSTATE_END_OF_LIST()
    },
};

static void lm48557_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    dc->vmsd = &vmstate_lm48557;
    device_class_set_legacy_reset(dc, lm48557_reset);
    k->recv = lm48557_recv;
    k->send = lm48557_send;
}

static const TypeInfo lm48557_info = {
    .name = TYPE_LM48557,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LM48557State),
    .class_init = lm48557_class_init,
};

static void lm48557_register_types(void)
{
    type_register_static(&lm48557_info);
}

type_init(lm48557_register_types)
