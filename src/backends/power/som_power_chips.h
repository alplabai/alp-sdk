/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * Binding of chip-driver contexts to SoM power domains (#2784, U5).  INTERNAL.
 *
 * Each call routes one domain through the driver that owns the chip, so the
 * driver's own state stays correct across the quiesce (e.g. the CC3501E
 * context's `initialised` flag).  Call it after the driver's init succeeded,
 * and keep the context alive for as long as the binding holds.  The default
 * pin / I2C action applies to any domain without a binding.
 */

#ifndef ALP_BACKENDS_POWER_SOM_POWER_CHIPS_H
#define ALP_BACKENDS_POWER_SOM_POWER_CHIPS_H

#include <alp/chips/cc3501e.h>
#include <alp/chips/rv3028c7.h>
#include <alp/chips/tmp112.h>
#include <alp/peripheral.h>

struct device;

/** WIFI_BLE through @p fw.  AUTO holds nRESET low and marks the context down;
 *  restore is cc3501e_hard_reset() (never cc3501e_reset()) and re-arms the
 *  context.  RAIL_OFF uses cc3501e_power_off(). */
alp_status_t alp_som_power_bind_cc3501e(cc3501e_t *fw);

/** TEMP_SENSOR through the TMP112 chip driver's shutdown bit. */
alp_status_t alp_som_power_bind_tmp112(tmp112_t *ctx);

/** RTC through the RV-3028 chip driver: CLKOUT routed low, nothing else. */
alp_status_t alp_som_power_bind_rv3028(rv3028c7_t *ctx);

/** EXT_FLASH: reset-hold plus a notice to flash_ospi_alif that the part is
 *  back in its power-on 1-1-1 framing.  Binds automatically at boot when the
 *  OSPI driver is built; exposed for the test. */
alp_status_t alp_som_power_bind_flash(const struct device *ospi);

#endif /* ALP_BACKENDS_POWER_SOM_POWER_CHIPS_H */
