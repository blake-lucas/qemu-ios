/* The iPod touch 3G's speaker volume from its LM48557 amplifier: table 6's gain for each code, the top level iOS
 * uses (+48 dB) at unity and the rest below it, mute and shutdown silent, and unity until iOS first writes it.
 * SLICE hw/arm/lm48557.c define LM48557_(VOLUME|MUTE|NORMAL|TOP_DB)[[:space:]]
 * SLICE hw/arm/lm48557.c fn lm48557_db lm48557_output_gain
 * CFLAGS -Wall -Wextra -Werror -lm
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
typedef struct {
    uint8_t mode, volume;
    bool written;
} LM48557State;
#define LM48557(obj) ((LM48557State *)(obj))
#include "slice.h"
static bool near(double gain, double db)
{
    return fabs(gain - pow(10, db / 20)) < 1e-12;
}
int main(void)
{
    /* Table 6 (SNAS486D): codes 1-7 step -25..-8, 8-0x1f in 2 dB steps to +40, 0x3c-0x3f +42..+48, and the codes
     * between repeat the low ones and then run 0, 4, 8 ... */
    static const struct { uint8_t code; int db; } table[] = {
        { 0x01, -25 }, { 0x07, -8 }, { 0x08, -6 }, { 0x0b, 0 }, { 0x0c, 2 }, { 0x1b, 32 }, { 0x1f, 40 },
        { 0x21, -25 }, { 0x29, -4 }, { 0x2a, 0 }, { 0x2b, 4 }, { 0x2f, 16 }, { 0x30, 18 }, { 0x3b, 40 },
        { 0x3c, 42 }, { 0x3f, 48 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        assert(lm48557_db(table[i].code) == table[i].db);
    }

    /* Reset: unity until iOS writes the amplifier (as before it was modelled, and a state saved then). */
    LM48557State s = { .mode = 0, .volume = LM48557_VOLUME };
    assert(lm48557_output_gain(&s) == 1.0);

    /* 8C148: 0x01 0xbf at the top (unity), 0x01 0x9b at level 8 (-16 dB), 0x01 0x8c at level 1 (-46 dB). */
    s.written = true;
    s.mode = LM48557_NORMAL;
    s.volume = 0xbf;
    assert(lm48557_output_gain(&s) == 1.0);
    s.volume = 0x9b;
    assert(near(lm48557_output_gain(&s), -16));
    s.volume = 0x8c;
    assert(near(lm48557_output_gain(&s), -46));

    /* Level 0 and sleep: 0x00 0x80 (shutdown, code 0); mute (0x03) and code 0x20 are silent too. */
    s.mode = 0;
    assert(lm48557_output_gain(&s) == 0.0);
    s.mode = LM48557_NORMAL | LM48557_MUTE;
    s.volume = 0xbf;
    assert(lm48557_output_gain(&s) == 0.0);
    s.mode = LM48557_NORMAL;
    s.volume = 0x80;
    assert(lm48557_output_gain(&s) == 0.0);
    s.volume = 0xa0;
    assert(lm48557_output_gain(&s) == 0.0);
    printf("lm48557-volume: ok\n");
    return 0;
}
