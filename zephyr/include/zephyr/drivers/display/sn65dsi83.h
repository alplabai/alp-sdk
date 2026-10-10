/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public, devicetree-independent API of the TI SN65DSI83 DSI-to-LVDS bridge
 * driver (zephyr/drivers/display/display_sn65dsi83.c, compatible
 * "ti,sn65dsi83").  The bridge implements no Zephyr display-class API of its
 * own (see the driver file header), so this header carries only the one
 * extra thing an application needs that the display API has no room for:
 * the bridge's own link-error status, CSR 0xE5.  ADR-0017-ADJACENT,
 * BENCH-UNVERIFIED.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_DISPLAY_SN65DSI83_H_
#define ZEPHYR_INCLUDE_DRIVERS_DISPLAY_SN65DSI83_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the SN65DSI83's latched link-error register (CSR 0xE5).
 *
 * Datasheet Section 7.4.2 (Initialization Sequence) clears this register at
 * the end of init; Section 7.4.4 (IRQ and Error Reporting) documents each
 * bit as CRC / DSI protocol / PLL-unlock / LVDS errors, latched (read/write-
 * 1-to-clear) rather than live.  A non-zero read after init, or after
 * display_blanking_off() re-starts the DPI feed, means the bridge saw a link
 * fault; the driver's init returns -ENODEV/-ETIMEDOUT for the faults it can
 * detect during its own sequence, but init running once at boot cannot see a
 * fault that starts after cdc200 begins scanning out -- that is what this
 * call is for.  Read it, do not assume a clean init means a clean link
 * forever.
 *
 * @param dev The ti,sn65dsi83 device.
 * @param e5  Out: CSR 0xE5 as read (bit meaning: see the datasheet; the
 *            driver does not decode individual bits -- it has no error-
 *            recovery policy of its own to attach to one).
 *
 * @retval 0       Read succeeded; @p e5 holds the register value (0 = no
 *                  latched error since the last read/write-1-to-clear).
 * @retval -EINVAL @p dev or @p e5 is NULL.  Not checked: passing a @p dev of
 *                  some OTHER driver class reads whatever its `config`
 *                  struct's first bytes happen to be as an i2c_dt_spec --
 *                  callers must pass a real ti,sn65dsi83 device.
 * @retval <0      The I2C transaction itself failed (bus/NAK).
 */
int sn65dsi83_read_errors(const struct device *dev, uint8_t *e5);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_DISPLAY_SN65DSI83_H_ */
