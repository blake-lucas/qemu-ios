/* The host's volume from the iPhone 4's and iPad's CS42L61 (CS42L58_VOLUME_NONE): on the speaker iOS scales the PCM
 * (0x21 at 0x00) except for a ringtone, whose Ringer level is 0x21 (0x14 at the top to 0xc2 at the bottom), heard
 * up to unity (the iPhone 4, "ringer"; the iPad's 0xed stays unity); with a headphone in, 0x22 as before, and the
 * speaker's level while the headphones are muted.
 * SLICE hw/arm/ipod_touch_cs42l58.c fn cs42l58_output_gain
 * CFLAGS -Wall -Wextra -Werror -lm
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
enum { CS42L58_VOLUME_NONE, CS42L58_VOLUME_CS42L59, CS42L58_VOLUME_CS42L61 };
typedef struct {
    uint8_t regs[128];
    bool hp;
    uint8_t volume;
    bool ringer;
} CS42L58State;
#include "slice.h"
static bool near(double gain, double db)
{
    return fabs(gain - pow(10, db / 20)) < 1e-12;
}
int main(void)
{
    /* The iPad parks 0x21 at 0xed (-9.5 dB) for its own sounds, which play at unity. */
    CS42L58State ipad = { .volume = CS42L58_VOLUME_NONE };
    ipad.regs[0x21] = 0xed;
    assert(cs42l58_output_gain(&ipad) == 1);
    CS42L58State s = { .volume = CS42L58_VOLUME_NONE, .ringer = true };

    /* Speaker: system sounds and media at 0x00 (and power-on's 0x34) play as iOS scaled them. */
    assert(cs42l58_output_gain(&s) == 1);
    s.regs[0x21] = 0x34;
    assert(cs42l58_output_gain(&s) == 1);
    /* A ringtone at the Ringer's top (+10 dB) stays at full scale; at its bottom it is 31 dB down. */
    s.regs[0x21] = 0x14;
    assert(cs42l58_output_gain(&s) == 1);
    s.regs[0x21] = 0xc2;
    assert(near(cs42l58_output_gain(&s), -31));
    s.regs[0x21] = 0xf0;
    assert(near(cs42l58_output_gain(&s), -8));
    /* Headphones: 0x22 unmuted is their level (top 0x02 at unity), whatever 0x21 holds. */
    s.hp = true;
    s.regs[0x22] = 0x02;
    assert(near(cs42l58_output_gain(&s), 0));
    s.regs[0x22] = 0x44;
    assert(near(cs42l58_output_gain(&s), -31));
    /* Headphones muted, the speaker playing (0x67 0x00 on the iPhone 4): the speaker's level. */
    s.regs[0x22] = 0x80 | 0x02;
    s.regs[0x63] = 0x3f;
    s.regs[0x67] = 0x00;
    assert(near(cs42l58_output_gain(&s), -8));
    s.regs[0x21] = 0x00;
    assert(cs42l58_output_gain(&s) == 1);
    /* Both muted (0x63 and 0x67 0x3f): silence. */
    s.regs[0x67] = 0x3f;
    assert(cs42l58_output_gain(&s) == 0);
    puts("PASS: CS42L61 (iPhone 4, iPad) speaker ringtone level, headphone level and mutes");
}
