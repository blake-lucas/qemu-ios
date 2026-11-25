/*
 * CS42L65 SPI register port, qualified against the original N42 driver.
 *
 * AppleARMSPIAudio's read/write methods (11A465, 809ee348/809ee3f8):
 *   6d|6c, register>>8, register&7f | autoincrement<<7, length-1, data...
 * AppleCS42L65Audio's identity probe (80b78d18) reads registers 1..3 and
 * checks 42 a6 5x. Other registers currently retain writes. Clocking, analog
 * controls and interrupt/status side effects remain to be qualified; this
 * register-port model does not claim a working codec or audio stream.
 *
 * Headphone jack (machine option headphones=on, LT_HEADPHONES=0/1): 14G61's AppleCS42L65Mikey is the generic
 * AppleCSCodecMikey (AppleCSEmbeddedAudio) over a register block at 0x6a (its accessors add 0x6a), the driver the
 * iPod touch 4G's CS42L59 answers at 0x2c. AppleEmbeddedAudio reads block register 7 (0x71) bit 7 as plugged, but
 * only when a jack interrupt asks it to look: status 0x75 bits 5-6 (mask 0x73), which the model raises once, when
 * the driver unmasks bit 5. Headset detection then writes 0x9e to 0x6e (bit 7: running; the iPod's 0x30 <- 0x9e)
 * and unmasks the type-detect interrupt (mask 0x74 bits 3-4 clear). The handler reads status 0x75 and 0x76 (bits
 * 3-4: type detect done; both read clear), then 0x71 and 0x72, and a set 0x72 bit 0 (no mic, no button)
 * classifies a plain headphone before the per-contact results are looked at: iOS routes to Headphones, plays
 * through i2s0 instead of the speaker amplifier's MCA0, and runs no speaker protection. The interrupt is DT
 * audio-codec "interrupts" d2/1: GPIO 210, active low. ponytail: read from the driver (AppleCSEmbeddedAudio
 * 0x80a5cc9c, 0x80a5cd26, 0x80a5d578, 0x80a5bb98 in 14G61's kernelcache), not from a datasheet.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/cs42l65.h"

OBJECT_DECLARE_SIMPLE_TYPE(CS42L65State, CS42L65)
#define CS42L65_REGS 65536

#define CS42L65_MIKEY_DETECT 0x6e   /* bit 7: headset type detection running (0x9e) */
#define CS42L65_MIKEY_JACK   0x71   /* bit 7: plugged */
#define CS42L65_MIKEY_COMP   0x72   /* bit 0: no mic (a plain headphone) */
#define CS42L65_MIKEY_MASK1  0x73   /* bit 5 clear: jack interrupt unmasked */
#define CS42L65_MIKEY_MASK2  0x74   /* bits 3-4 clear: type-detect interrupt unmasked */
#define CS42L65_MIKEY_INT1   0x75   /* bits 5-6: the jack changed (read clears) */
#define CS42L65_MIKEY_INT2   0x76   /* bits 3-4: type detect done (read clears) */
#define CS42L65_INT_JACK     0x20
#define CS42L65_REG_RATE     0x12   /* bits 0-3: the I2S frame rate */
#define CS42L65_REG_HP_VOL   0x246  /* the headphone output's level, signed 1 dB steps, 0x40 muted */
#define CS42L65_INT_TYPE     0x18

struct CS42L65State {
    SSIPeripheral parent_obj;
    uint8_t regs[CS42L65_REGS];
    uint8_t command, header_pos, high;
    bool autoincrement;
    uint16_t address, remaining;
    unsigned bad_commands;
    bool hp;               /* a plain headphone in the jack (cs42l65_set_headphones) */
    bool hp_announced;     /* its jack interrupt was raised */
    uint8_t hp_status1;    /* pending 0x75 status */
    uint8_t hp_status;     /* pending 0x76 status */
    qemu_irq irq;          /* the codec's interrupt, 1 = asserted */
    Clock *lrclk;          /* out: the frame clock of its I2S port (i2s0), which it masters */
};

/*
 * 14G61 AppleCS42L65Audio's setSampleRate (0x81040e40) writes these codes to register 0x12 bits 0-3; the codec
 * masters i2s0's clocks (its DT audio-data TX config), so the port plays at the rate set here: 44.1 kHz in games.
 */
static unsigned cs42l65_rate(uint8_t code)
{
    static const unsigned rates[16] = { 0, 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };

    return rates[code & 0xf];
}

/*
 * The jack's interrupt once the driver unmasks it (the headphone was there from power-on); then type detection
 * running with its interrupt unmasked, which a plain headphone answers at once.
 */
static void cs42l65_hp_detect(CS42L65State *s)
{
    if (!s->hp) {
        return;
    }
    if (!s->hp_announced && !(s->regs[CS42L65_MIKEY_MASK1] & CS42L65_INT_JACK)) {
        s->hp_announced = true;
        s->hp_status1 |= CS42L65_INT_JACK;
    }
    if ((s->regs[CS42L65_MIKEY_DETECT] & 0x80) && !(s->regs[CS42L65_MIKEY_MASK2] & CS42L65_INT_TYPE)) {
        s->hp_status |= CS42L65_INT_TYPE;
    }
    qemu_set_irq(s->irq, (s->hp_status1 | s->hp_status) != 0);
}

static uint8_t cs42l65_read(CS42L65State *s, uint16_t reg)
{
    uint8_t v = s->regs[reg];

    if (!s->hp) {
        return v;
    }
    switch (reg) {
    case CS42L65_MIKEY_JACK:
        return v | 0x80;
    case CS42L65_MIKEY_COMP:
        return v | 0x01;
    case CS42L65_MIKEY_INT1:
        v = s->hp_status1;
        s->hp_status1 = 0;
        qemu_set_irq(s->irq, s->hp_status != 0);
        return v;
    case CS42L65_MIKEY_INT2:
        v = s->hp_status;
        s->hp_status = 0;
        qemu_set_irq(s->irq, s->hp_status1 != 0);
        return v;
    default:
        return v;
    }
}

/*
 * The headphone output's level, which the host hears: on the Headphones route iOS leaves the PCM at full scale for
 * the upper volume steps and sets 0x246 (14G61 AppleCS42L65Audio 0x810415f8: the step's dB, clamped to +12..-50,
 * then & 0x7f; 0x40 when muted), +2 at the top, -1, -4, -8 ... -30 dB as the buttons step down, below which it
 * scales the PCM itself. The finer 0.5 dB part in 0x244 is left out. The top plays at unity, as the iPods' codecs.
 */
double cs42l65_output_gain(void *opaque)
{
    CS42L65State *s = CS42L65(opaque);
    uint8_t v = s->regs[CS42L65_REG_HP_VOL] & 0x7f;

    if (v == 0x40) {
        return 0.0;
    }
    return pow(10.0, MIN((int8_t)(v << 1) / 2 - 2, 0) / 20.0);
}

void cs42l65_set_headphones(DeviceState *dev, bool on)
{
    CS42L65(dev)->hp = on;
}

static uint32_t cs42l65_transfer(SSIPeripheral *dev, uint32_t value)
{
    CS42L65State *s = CS42L65(dev);
    uint8_t byte = value;
    if (s->remaining) {
        uint8_t result = 0;
        if (s->command == 0x6d) {
            result = cs42l65_read(s, s->address);
        } else if (s->address < 1 || s->address > 3) {
            s->regs[s->address] = byte;
            if (s->address == CS42L65_REG_RATE && cs42l65_rate(byte)) {
                clock_update_hz(s->lrclk, cs42l65_rate(byte));
            }
            if (s->address == CS42L65_MIKEY_DETECT || s->address == CS42L65_MIKEY_MASK1 ||
                s->address == CS42L65_MIKEY_MASK2) {
                cs42l65_hp_detect(s);
            }
        }
        if (s->autoincrement) {
            s->address = (s->address + 1) & (CS42L65_REGS - 1);
        }
        if (!--s->remaining) {
            s->header_pos = 0;
        }
        return result;
    }
    switch (s->header_pos) {
    case 0:
        if (byte != 0x6c && byte != 0x6d) {
            if (s->bad_commands++ < 8) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "cs42l65: unknown command 0x%02x\n", byte);
            }
            return 0;
        }
        s->command = byte;
        break;
    case 1:
        s->high = byte;
        break;
    case 2:
        s->address = ((uint16_t)s->high << 8) | (byte & 0x7f);
        s->autoincrement = byte & 0x80;
        break;
    case 3:
        s->remaining = (uint16_t)byte + 1;
        break;
    default:
        g_assert_not_reached();
    }
    s->header_pos++;
    return 0;
}

static void cs42l65_reset(DeviceState *dev)
{
    CS42L65State *s = CS42L65(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[1] = 0x42;
    s->regs[2] = 0xa6;
    s->regs[3] = 0x50;
    s->command = s->header_pos = s->high = 0;
    s->autoincrement = false;
    s->address = s->remaining = 0;
    s->bad_commands = 0;
    s->hp_status = s->hp_status1 = 0;
    s->hp_announced = false;
}

static bool cs42l65_hp_needed(void *opaque)
{
    return CS42L65(opaque)->hp;
}

static const VMStateDescription vmstate_cs42l65_hp = {
    .name = "cs42l65/hp",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = cs42l65_hp_needed,
    .fields = (const VMStateField[]){VMSTATE_UINT8(hp_status, CS42L65State),
                                     VMSTATE_UINT8(hp_status1, CS42L65State),
                                     VMSTATE_BOOL(hp_announced, CS42L65State), VMSTATE_END_OF_LIST()}};

static int cs42l65_post_load(void *opaque, int version_id)
{
    CS42L65State *s = opaque;

    if (cs42l65_rate(s->regs[CS42L65_REG_RATE])) {
        clock_update_hz(s->lrclk, cs42l65_rate(s->regs[CS42L65_REG_RATE]));
    }
    return 0;
}

static const VMStateDescription vmstate_cs42l65 = {
    .name = TYPE_CS42L65,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cs42l65_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_UINT8_ARRAY(regs, CS42L65State, CS42L65_REGS),
        VMSTATE_UINT8(command, CS42L65State),
        VMSTATE_UINT8(header_pos, CS42L65State),
        VMSTATE_UINT8(high, CS42L65State),
        VMSTATE_BOOL(autoincrement, CS42L65State),
        VMSTATE_UINT16(address, CS42L65State),
        VMSTATE_UINT16(remaining, CS42L65State), VMSTATE_END_OF_LIST()},
    .subsections = (const VMStateDescription *const[]){&vmstate_cs42l65_hp, NULL}};

static void cs42l65_realize(SSIPeripheral *dev, Error **errp)
{
    cs42l65_reset(DEVICE(dev));
}

static void cs42l65_init(Object *obj)
{
    qdev_init_gpio_out(DEVICE(obj), &CS42L65(obj)->irq, 1);
    CS42L65(obj)->lrclk = qdev_init_clock_out(DEVICE(obj), "lrclk");
}

static void cs42l65_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(klass);
    sc->realize = cs42l65_realize;
    sc->transfer = cs42l65_transfer;
    sc->cs_polarity = SSI_CS_NONE; /* packets carry their own byte count */
    device_class_set_legacy_reset(dc, cs42l65_reset);
    dc->vmsd = &vmstate_cs42l65;
}

static const TypeInfo cs42l65_info = {
    .name = TYPE_CS42L65,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(CS42L65State),
    .instance_init = cs42l65_init,
    .class_init = cs42l65_class_init,
};
static void cs42l65_register_types(void)
{
    type_register_static(&cs42l65_info);
}
type_init(cs42l65_register_types)
