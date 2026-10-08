/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Power-management notice for the Alif OSPI flash driver (flash_ospi_alif.c).
 */

#ifndef ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H
#define ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tell the driver the NOR part was reset behind its back.
 *
 * The driver switches the part to Octal DDR on the first write / erase and
 * then reads in that framing.  A hardware reset of the part (the SoM power
 * layer holds OSPI1_RESETn low around STOP) returns it to its power-on 1-1-1
 * SPI framing, so the driver must forget the switch or it keeps speaking
 * Octal DDR to a part that no longer answers it.  The next write / erase
 * switches the part again.  The driver exposes no deep-power-down command.
 *
 * @param dev  The OSPI flash device.
 * @return 0.
 */
int flash_ospi_alif_reset_notify(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* ALP_ZEPHYR_DRIVERS_FLASH_FLASH_OSPI_ALIF_H */
