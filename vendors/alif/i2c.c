/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Alif Ensemble I2C wrapper for <alp/peripheral.h>.
 *
 * Active when:
 *   - ALP_OS=baremetal
 *   - ALP_SOM=aen
 *   - CMake option ALP_HAS_ALIF_HAL=ON (sets ALP_VENDOR_OVERRIDES_PERIPHERAL)
 *
 * The wrapper rides on Alif's CMSIS-Driver layer (`ARM_DRIVER_I2C`)
 * which is the canonical bare-metal Ensemble entry point per the
 * AN0011 application note.  Each studio-resolved bus_id maps onto
 * the matching driver instance (Driver_I2C0..Driver_I2C7).
 *
 * v0.2 scaffolding -- the body is gated on ALP_HAS_ALIF_HAL so the
 * file compiles to an empty translation unit when the HAL isn't
 * pulled in (CI's baremetal-aen scenario, today).  v0.2.x flips
 * the flag once cpackget AlifSemiconductor::Ensemble lands and the
 * real Driver_I2CN symbols become resolvable.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_ALIF_HAL)

#include <stddef.h>
#include <stdint.h>

#include "Driver_I2C.h"

extern ARM_DRIVER_I2C        Driver_I2C0;
extern ARM_DRIVER_I2C        Driver_I2C1;
extern ARM_DRIVER_I2C        Driver_I2C2;
extern ARM_DRIVER_I2C        Driver_I2C3;

static ARM_DRIVER_I2C *const alp_alif_i2c_drivers[] = {
    &Driver_I2C0,
    &Driver_I2C1,
    &Driver_I2C2,
    &Driver_I2C3,
};

static alp_status_t alif_to_alp(int32_t st)
{
    switch (st) {
    case ARM_DRIVER_OK:
        return ALP_OK;
    case ARM_DRIVER_ERROR_BUSY:
        return ALP_ERR_BUSY;
    case ARM_DRIVER_ERROR_TIMEOUT:
        return ALP_ERR_TIMEOUT;
    case ARM_DRIVER_ERROR_PARAMETER:
        return ALP_ERR_INVAL;
    case ARM_DRIVER_ERROR_UNSUPPORTED:
        return ALP_ERR_NOSUPPORT;
    default:
        return ALP_ERR_IO;
    }
}

static alp_status_t z_last_error;

alp_i2c_t          *alp_i2c_open(const alp_i2c_config_t *cfg)
{
    if (cfg == NULL) {
        z_last_error = ALP_ERR_INVAL;
        return NULL;
    }
    if (cfg->bus_id >= sizeof(alp_alif_i2c_drivers) / sizeof(alp_alif_i2c_drivers[0])) {
        z_last_error = ALP_ERR_OUT_OF_RANGE;
        return NULL;
    }
    ARM_DRIVER_I2C *d = alp_alif_i2c_drivers[cfg->bus_id];
    if (d == NULL) {
        z_last_error = ALP_ERR_NOT_READY;
        return NULL;
    }
    if (d->Initialize(NULL) != ARM_DRIVER_OK) {
        z_last_error = ALP_ERR_IO;
        return NULL;
    }
    if (d->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
        z_last_error = ALP_ERR_IO;
        return NULL;
    }
    int32_t speed = (cfg->bitrate_hz >= 1000000)  ? ARM_I2C_BUS_SPEED_FAST_PLUS
                    : (cfg->bitrate_hz >= 400000) ? ARM_I2C_BUS_SPEED_FAST
                                                  : ARM_I2C_BUS_SPEED_STANDARD;
    if (d->Control(ARM_I2C_BUS_SPEED, (uint32_t)speed) != ARM_DRIVER_OK) {
        z_last_error = ALP_ERR_IO;
        return NULL;
    }
    /* Opaque cast -- consumers never dereference alp_i2c_t. */
    return (alp_i2c_t *)d;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_I2C *)bus)->MasterTransmit(addr, data, (uint32_t)len, false));
}

alp_status_t alp_i2c_read(alp_i2c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_I2C *)bus)->MasterReceive(addr, data, (uint32_t)len, false));
}

alp_status_t alp_i2c_write_read(alp_i2c_t *bus, uint8_t addr, const uint8_t *wdata, size_t wlen,
                                uint8_t *rdata, size_t rlen)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    ARM_DRIVER_I2C *d  = (ARM_DRIVER_I2C *)bus;
    int32_t         st = d->MasterTransmit(addr, wdata, (uint32_t)wlen, true);
    if (st != ARM_DRIVER_OK) return alif_to_alp(st);
    return alif_to_alp(d->MasterReceive(addr, rdata, (uint32_t)rlen, false));
}

void alp_i2c_close(alp_i2c_t *bus)
{
    if (bus == NULL) return;
    ARM_DRIVER_I2C *d = (ARM_DRIVER_I2C *)bus;
    (void)d->PowerControl(ARM_POWER_OFF);
    (void)d->Uninitialize();
}

alp_status_t alp_last_error(void)
{
    return z_last_error;
}

#endif /* ALP_HAS_ALIF_HAL */
