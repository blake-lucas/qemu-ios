/* Production Cirrus I2C MAP framing and the native N42 identity transaction.
 * SLICE hw/arm/cs35l19.c range struct CS35L19State { | static int cs35l19_event
 * SLICE hw/arm/cs35l19.c fn cs35l19_event cs35l19_recv cs35l19_send cs35l19_reset
 * SLICE hw/arm/cs35l19.c define CS35L19_REG_VOLUME
 * SLICE hw/arm/cs35l19.c fn cs35l19_output_gain
 * CFLAGS -Wall -Wextra -Werror -lm
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct {
    int unused;
} I2CSlave;
typedef I2CSlave DeviceState;
typedef struct CS35L19State CS35L19State;
enum i2c_event { I2C_START_SEND, I2C_START_RECV, I2C_FINISH };
#define CS35L19(p) ((CS35L19State *)(p))
#define qemu_log(...) ((void)0)
#include "slice.h"
static void map(CS35L19State *s, uint8_t address)
{
    cs35l19_event((I2CSlave *)s, I2C_START_SEND);
    assert(!cs35l19_send((I2CSlave *)s, address));
}
int main(void)
{
    CS35L19State s;
    I2CSlave *bus = (I2CSlave *)&s;
    cs35l19_reset((DeviceState *)&s);
    map(&s, 0x81);
    cs35l19_event(bus, I2C_START_RECV);
    assert(cs35l19_recv(bus) == 0x35);
    assert(cs35l19_recv(bus) == 0xa1);
    assert(cs35l19_recv(bus) == 0x90);
    map(&s, 0x90);
    cs35l19_send(bus, 0xd2);
    cs35l19_send(bus, 0x19);
    map(&s, 0x90);
    cs35l19_event(bus, I2C_START_RECV);
    assert(cs35l19_recv(bus) == 0xd2);
    assert(cs35l19_recv(bus) == 0x19);
    map(&s, 0x81);
    cs35l19_send(bus, 0);
    cs35l19_send(bus, 0);
    cs35l19_send(bus, 0);
    map(&s, 0x81);
    assert(cs35l19_recv(bus) == 0x35);
    assert(cs35l19_recv(bus) == 0xa1);
    assert(cs35l19_recv(bus) == 0x90);
    map(&s, 0x20);
    cs35l19_send(bus, 7);
    cs35l19_send(bus, 9);
    cs35l19_event(bus, I2C_FINISH);
    map(&s, 0x20);
    assert(cs35l19_recv(bus) == 9);
    assert(cs35l19_recv(bus) == 9);
    map(&s, 0xff);
    for (unsigned i = 0; i < 256; i++)
        cs35l19_send(bus, i);
    assert(s.address == 127);
    cs35l19_reset((DeviceState *)&s);
    assert(!s.regs[0x20]);
    /* The digital volume 14G61 sets for a ringtone: 0x00 at the Ringer's top, 0xe8 (-12 dB) half way, 0xba
     * (-35 dB) at the bottom; 0x19 (written around every power change) and the rest of 0x19-0x33 mute. */
    const struct { uint8_t reg; double db; } volume[] = {
        { 0x00, 0 }, { 0xf5, -5.5 }, { 0xe8, -12 }, { 0xd5, -21.5 }, { 0xba, -35 }, { 0x34, -102 }, { 0x7f, -64.5 },
        { 0x80, -64 }, { 0xff, -0.5 }, { 0x18, 12 },
    };
    for (unsigned i = 0; i < sizeof(volume) / sizeof(volume[0]); i++) {
        s.regs[0x10] = volume[i].reg;
        assert(fabs(cs35l19_output_gain(&s) - pow(10, volume[i].db / 20)) < 1e-12);
    }
    for (unsigned v = 0x19; v <= 0x33; v++) {
        s.regs[0x10] = v;
        assert(cs35l19_output_gain(&s) == 0);
    }
    cs35l19_reset((DeviceState *)&s);
    assert(cs35l19_output_gain(&s) == 1); /* untouched, as before the guest writes it */
    puts("CS35L19 native ID/MAP/repeated-start/register/boundary/volume checks PASS");
}
