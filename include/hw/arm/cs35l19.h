/* The iPhone 5's CS35L19 speaker amplifier (hw/arm/cs35l19.c), an I2C device the board creates by type name. */
#ifndef HW_ARM_CS35L19_H
#define HW_ARM_CS35L19_H

#define TYPE_CS35L19 "cs35l19"

/* Its digital volume as a gain for the host (the speaker port's gain source); opaque is the device. */
double cs35l19_output_gain(void *opaque);

#endif
