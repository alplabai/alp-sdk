/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microchip ATECC608B secure element driver (I²C lifecycle).
 * See <alp/chips/atecc608b.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/atecc608b.h"

#define ATECC608B_CMD_IDLE  0x02u
#define ATECC608B_CMD_SLEEP 0x01u

alp_status_t atecc608b_init(atecc608b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
	if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
	if (i2c_addr == 0) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->bus         = bus;
	dev->addr        = i2c_addr;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t atecc608b_wake(atecc608b_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	/* Real wake: SDA low > 60 us, but the portable I2C API can't
     * hold a line low directly.  Substitute: dummy write to addr 0
     * which the bus stalls on, achieves the same effect in many
     * real I2C controllers.  Full hardware-specific wake path
     * lives in CryptoAuthLib's HAL backend (TBD). */
	uint8_t buf = 0;
	(void)alp_i2c_write(dev->bus, 0x00, &buf, 1);
	/* tWHI: the part needs >= 1500 us after the wake pulse before it answers.
	 * alp_delay_ms has millisecond granularity, so this rounds UP to 2 ms --
	 * the contract is a minimum hold and overshooting is safe (the device
	 * stays awake until an idle/sleep command or its ~1.3 s watchdog).
	 * Rounding DOWN to 1 ms would break the datasheet minimum. */
	alp_delay_ms(2);
	return ALP_OK;
}

alp_status_t atecc608b_idle(atecc608b_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	uint8_t buf = ATECC608B_CMD_IDLE;
	return alp_i2c_write(dev->bus, dev->addr, &buf, 1);
}

alp_status_t atecc608b_sleep(atecc608b_t *dev)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	uint8_t buf = ATECC608B_CMD_SLEEP;
	return alp_i2c_write(dev->bus, dev->addr, &buf, 1);
}

void atecc608b_deinit(atecc608b_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->bus         = NULL;
}
