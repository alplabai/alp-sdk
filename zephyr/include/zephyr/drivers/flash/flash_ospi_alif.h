/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Power-management notice for the Alif OSPI flash driver (flash_ospi_alif.c).
 */

#ifndef ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H
#define ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Take the flash out of service so its RESETn can be asserted safely.
 *
 * Takes the driver lock (blocking up to @p timeout_ms), then waits for any
 * program or erase already in the part to finish (WIP clear).  On success the
 * lock STAYS HELD: no read, write or erase can start until
 * flash_ospi_alif_resume().  The caller asserts RESETn only after this returns
 * 0; resetting the part with a write in flight would leave the sector torn.
 *
 * The same thread must call flash_ospi_alif_resume() (the lock is a mutex).
 *
 * @param dev         The OSPI flash device.
 * @param timeout_ms  How long to wait for a transfer in progress.
 * @return 0 (lock held); -EBUSY when a transfer holds the lock past the
 *         timeout; -ETIMEDOUT / -EIO when the part never reports ready.  On a
 *         non-zero return the lock is NOT held.
 */
int flash_ospi_alif_suspend(const struct device *dev, uint32_t timeout_ms);

/**
 * @brief Put the flash back in service after its RESETn was released.
 *
 * The part is back in its power-on 1-1-1 SPI framing, so the driver forgets
 * its Octal DDR switch (the next write or erase switches the part again) and
 * releases the lock taken by flash_ospi_alif_suspend().  The driver exposes no
 * deep-power-down command.
 *
 * @param dev  The OSPI flash device.
 * @return 0.
 */
int flash_ospi_alif_resume(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H */
