#ifndef HW_CD3272_MIKEY_H
#define HW_CD3272_MIKEY_H

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "time.h"

#define TYPE_CD3272MIKEY                 "cd3272mikey"
OBJECT_DECLARE_SIMPLE_TYPE(CD3272MikeyState, CD3272MIKEY)

typedef struct CD3272MikeyState {
	I2CSlave i2c;
	uint32_t cmd;        /* the MAP: register pointer, bit 7 auto-increment */
	bool have_cmd;       /* the MAP byte of this write has been received */
	uint8_t regs[128];
	/* headphone jack, the "headphones" property only */
	bool hp;             /* a plain headphone is plugged in */
	bool irq_level;      /* the interrupt line, 1 = asserted */
	qemu_irq irq;
} CD3272MikeyState;

#endif
