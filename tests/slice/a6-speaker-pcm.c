/* What the host hears from an MCA port: 24-bit samples right-aligned in 32-bit words (+0x08's low byte, the unused
 * high bits) fill the S32 voice, and the gain source's volume scales either width; 16-bit samples at unity pass as
 * they came.
 * SLICE hw/arm/s5l8930_i2s.c fn i2s_out_cb
 * CFLAGS -Wall -Wextra -Werror -Wno-unused-parameter -lm
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define I2S_RING 64
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
typedef struct CS42L58State CS42L58State;
static double cs42l58_output_gain(CS42L58State *s)
{
    assert(0);
}
typedef struct {
    bool mca;
    uint32_t regs[1024];
    unsigned out_bytes;
    CS42L58State *codec;
    double (*gain)(void *opaque);
    void *gain_opaque;
    uint8_t ring[I2S_RING];
    uint32_t tail, level;
    void *voice, *audio_be;
} S5L8930I2SState;
static uint16_t lduw_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8;
}
static int ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return (int)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}
static void stw_le_p(void *p, uint16_t v)
{
    memcpy(p, (uint8_t[]){ v, v >> 8 }, 2);
}
static void stl_le_p(void *p, uint32_t v)
{
    memcpy(p, (uint8_t[]){ v, v >> 8, v >> 16, v >> 24 }, 4);
}
static uint8_t played[256];
static unsigned count;
static size_t audio_be_write(void *be, void *voice, void *data, size_t size)
{
    assert(count + size <= sizeof(played));
    memcpy(played + count, data, size);
    count += size;
    return size;
}
static double volume;
static double gain(void *opaque)
{
    return volume;
}
#include "slice.h"
static void put32(S5L8930I2SState *s, uint32_t v)
{
    stl_le_p(s->ring + (s->tail + s->level) % I2S_RING, v);
    s->level += 4;
}
static int32_t out32(unsigned i)
{
    return ldl_le_p(played + i * 4);
}
int main(void)
{
    S5L8930I2SState s = { .mca = true, .out_bytes = 4, .gain = gain };

    /* 14G61's ringtone on the speaker with a headphone in: +0x08 0x1008, 24-bit words with the top byte 0, so a
     * negative sample (0xe6b5cc, -1657396) must not play as a small positive one. */
    s.regs[8 >> 2] = 0x1008;
    volume = 1;
    put32(&s, 0x00e6b5cc);
    put32(&s, 0x00194a34);
    put32(&s, 0x007fffff);
    put32(&s, 0x00800000);
    i2s_out_cb(&s, 64);
    assert(count == 16 && !s.level && s.tail == 16);
    assert(out32(0) == -1657396 * 256 && out32(1) == 1657396 * 256);
    assert(out32(2) == 0x7fffff00 && out32(3) == INT32_MIN);
    /* The amplifier's volume (-12 dB, the Ringer half way) scales the whole word. */
    count = 0;
    volume = pow(10, -12 / 20.0);
    put32(&s, 0x00194a34);
    i2s_out_cb(&s, 64);
    assert(count == 4 && out32(0) == (int32_t)llrint(1657396 * 256 * volume));
    /* Muted (0x19 between sounds): silence, the ring still drained. */
    count = 0;
    volume = 0;
    put32(&s, 0x007fffff);
    i2s_out_cb(&s, 64);
    assert(count == 4 && out32(0) == 0 && !s.level);
    /* Words on both sides of the ring's end, each shifted whole. */
    count = 0;
    volume = 1;
    s.tail = I2S_RING - 4;
    s.level = 0;
    put32(&s, 0x00000100);
    put32(&s, 0x00000200);
    i2s_out_cb(&s, 64);
    assert(count == 8 && out32(0) == 0x10000 && out32(1) == 0x20000 && !s.level && s.tail == 4);

    /* The speaker route's 16-bit stream (+0x08 0x1010, S16): at unity as it came, scaled with a volume. */
    S5L8930I2SState t = { .mca = true, .out_bytes = 2, .gain = gain };
    t.regs[8 >> 2] = 0x1010;
    count = 0;
    volume = 1;
    stw_le_p(t.ring, 0x8000);
    stw_le_p(t.ring + 2, 0x1234);
    t.level = 4;
    i2s_out_cb(&t, 64);
    assert(count == 4 && lduw_le_p(played) == 0x8000 && lduw_le_p(played + 2) == 0x1234);
    count = 0;
    volume = 0.5;
    stw_le_p(t.ring + 4, 0x8000);
    stw_le_p(t.ring + 6, 0x1234);
    t.level = 4;
    i2s_out_cb(&t, 64);
    assert(count == 4 && (int16_t)lduw_le_p(played) == -16384 && lduw_le_p(played + 2) == 0x91a);
    puts("PASS: MCA 24-bit words fill the S32 voice, the gain source scales both widths, S16 at unity unchanged");
}
