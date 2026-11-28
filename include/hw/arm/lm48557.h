#ifndef HW_ARM_LM48557_H
#define HW_ARM_LM48557_H

/* National LM48557 speaker amplifier, the iPod touch 3G's (DT i2c2/spkr-amp, I2C 0x6f). */
#define TYPE_LM48557 "lm48557"

/* The linear gain its volume (and mute) applies to what the speaker plays, relative to the top level. */
double lm48557_output_gain(void *opaque);

#endif
