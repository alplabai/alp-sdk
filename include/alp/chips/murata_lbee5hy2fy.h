/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file murata_lbee5hy2fy.h
 * @brief Murata LBEE5HY2FY-922 Wi-Fi 6 + BLE 5.4 module thin GPIO surface.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * The LBEE5HY2FY-922 (Type 2FY) wraps the Infineon CYW55513 dual-band
 * combo silicon.  Air-side traffic rides SDIO (Wi-Fi) + UART (BT)
 * + I2S (BT audio); this driver covers only the **side-channel GPIO
 * lines** that the host has to manage:
 *
 *   - `BT_REG_ON`     -- BT subsystem regulator enable.  Drive high
 *                        to power BT on; low for off.  Active-high.
 *   - `WL_REG_ON`     -- Wi-Fi subsystem regulator enable.  Same
 *                        polarity.
 *   - `BT_HOST_WAKE`  -- module wakes host on BT activity (input).
 *   - `WL_HOST_WAKE`  -- module wakes host on Wi-Fi activity (input).
 *   - `BT_DEV_WAKE`   -- host wakes module's BT subsystem (output).
 *                        **Intentionally not routed on V2N** --
 *                        BT power-management uses the BT_HOST_WAKE
 *                        line only.  Driver helper returns
 *                        `ALP_ERR_NOSUPPORT` when the handle is NULL.
 *
 * The SDIO + UART + I2S data paths are owned by the OS stack
 * (Yocto/Linux brcmfmac + BlueZ on V2N).  This driver doesn't try
 * to be a full stack -- it just owns the four (five) GPIO lines.
 *
 * @par V2N board wiring
 *
 * | Line          | Side    | Pad     | Notes                                 |
 * |---------------|---------|---------|---------------------------------------|
 * | BT_REG_ON     | GD32    | `PE14`  | accessed via gd32g553_gpio_write(...) |
 * | WL_REG_ON     | GD32    | `PE15`  | accessed via gd32g553_gpio_write(...) |
 * | BT_HOST_WAKE  | Renesas | `P05`   | input on the V2N RZ/V2N               |
 * | WL_HOST_WAKE  | Renesas | `P72`   | input on the V2N RZ/V2N               |
 | BT_DEV_WAKE   | —       | —       | **Intentionally not routed on V2N**;  |
 * |               |         |         | pass NULL to the init helper.         |
 *
 * Because the two `REG_ON` lines live on the GD32 supervisor MCU
 * (the V2N module's companion -- see the GD32 bridge protocol
 * document under `docs/gd32-bridge-protocol.md`)
 * and the host cannot reach them through `alp_gpio_t`, the driver
 * takes **caller-supplied set/get callbacks** for those lines.
 * Boards point the callbacks at `gd32g553_gpio_write` (V2N) or at
 * a direct `alp_gpio_*` wrapper (boards that route the REG_ON pins
 * straight to the host SoC).
 *
 * The two `HOST_WAKE` lines are normal host-SoC inputs, so they
 * arrive as plain `alp_gpio_t *` handles.  Pass NULL if a board does
 * not wire either signal (the wake polling falls back to "always
 * report low").
 *
 * @par Datasheet provenance
 * - Murata LBEE5HY2FY product brief + Type2FY pinout.
 * - Infineon CYW55513 datasheet (NDA) -- silicon under the lid.
 */

#ifndef ALP_CHIPS_MURATA_LBEE5HY2FY_H
#define ALP_CHIPS_MURATA_LBEE5HY2FY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Identifier for each of the two regulator-enable lines. */
typedef enum {
	MURATA_REG_BT = 0, /**< `BT_REG_ON`. */
	MURATA_REG_WL = 1, /**< `WL_REG_ON`. */
} murata_reg_t;

/** Set the regulator-enable line at @p which to the active-high level
 *  @p enable.  @return 0 on success, negative on error. */
typedef int (*murata_reg_set_t)(murata_reg_t which, bool enable, void *user);

/** Read back the current driver-side cached state of @p which. */
typedef int (*murata_reg_get_t)(murata_reg_t which, bool *enable, void *user);

/** Driver context. */
typedef struct {
	bool initialised;
	/* REG_ON outputs (GD32-side on V2N) -- callback driven. */
	murata_reg_set_t reg_set;
	murata_reg_get_t reg_get;
	void            *reg_user;
	/* HOST_WAKE inputs (Renesas-side on V2N) -- direct alp_gpio_t. */
	alp_gpio_t *bt_host_wake; /**< NULL if the board doesn't route this. */
	alp_gpio_t *wl_host_wake; /**< NULL if the board doesn't route this. */
	alp_gpio_t *bt_dev_wake;  /**< NULL today (TBD on V2N). */
	/* Cached state. */
	bool bt_powered;
	bool wl_powered;
} murata_lbee5hy2fy_t;

/**
 * @brief Initialise the driver in the OFF state.
 *
 * Drives both REG_ON lines low (powering down the module) by
 * invoking @p reg_set twice.  Caller MUST have set up the callbacks
 * + @p reg_user before calling, and the HOST_WAKE GPIO handles
 * (where wired) MUST already be configured as inputs.
 *
 * @param ctx           Driver context (output).
 * @param reg_set       Required.  Callback used to drive REG_ON outputs.
 * @param reg_get       Optional (may be NULL).  When NULL the driver
 *                      relies on its own cached @c bt_powered /
 *                      @c wl_powered fields for read-back.
 * @param reg_user      Opaque passed back to the callbacks.
 * @param bt_host_wake  Optional alp_gpio_t for BT_HOST_WAKE input.
 * @param wl_host_wake  Optional alp_gpio_t for WL_HOST_WAKE input.
 * @param bt_dev_wake   Optional alp_gpio_t for BT_DEV_WAKE output;
 *                      NULL on V2N until the maintainer routes it.
 */
alp_status_t murata_lbee5hy2fy_init(murata_lbee5hy2fy_t *ctx,
                                    murata_reg_set_t     reg_set,
                                    murata_reg_get_t     reg_get,
                                    void                *reg_user,
                                    alp_gpio_t          *bt_host_wake,
                                    alp_gpio_t          *wl_host_wake,
                                    alp_gpio_t          *bt_dev_wake);

/** @brief Drive `BT_REG_ON` to the requested level. */
alp_status_t murata_lbee5hy2fy_bt_power(murata_lbee5hy2fy_t *ctx, bool on);

/** @brief Drive `WL_REG_ON` to the requested level. */
alp_status_t murata_lbee5hy2fy_wl_power(murata_lbee5hy2fy_t *ctx, bool on);

/**
 * @brief Pulse `BT_DEV_WAKE` to wake the BT subsystem from sleep.
 *
 * @return ALP_OK on success.
 * @return ALP_ERR_NOSUPPORT if the line isn't wired on this board. */
alp_status_t murata_lbee5hy2fy_bt_wake_device(murata_lbee5hy2fy_t *ctx);

/** @brief Read the BT_HOST_WAKE pin level (true = module asserting wake). */
alp_status_t murata_lbee5hy2fy_bt_host_wake_level(murata_lbee5hy2fy_t *ctx, bool *level);

/** @brief Read the WL_HOST_WAKE pin level. */
alp_status_t murata_lbee5hy2fy_wl_host_wake_level(murata_lbee5hy2fy_t *ctx, bool *level);

/** @brief Release the context.  Idempotent.  Powers the module down
 *         by driving both REG_ON lines low before returning. */
void murata_lbee5hy2fy_deinit(murata_lbee5hy2fy_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MURATA_LBEE5HY2FY_H */
