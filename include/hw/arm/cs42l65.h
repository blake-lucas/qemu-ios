/* The iPhone 5's CS42L65 codec (hw/arm/cs42l65.c): an SSI peripheral the SPI3 controller creates by type name. */
#ifndef HW_ARM_CS42L65_H
#define HW_ARM_CS42L65_H

#include "hw/core/qdev.h"

#define TYPE_CS42L65 "cs42l65"

/* Report a plain headphone in the jack (before the guest starts); its interrupt is the device's GPIO out 0. */
void cs42l65_set_headphones(DeviceState *dev, bool on);

#endif
