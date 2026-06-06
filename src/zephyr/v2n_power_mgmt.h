/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N supervisor power-management auxiliary: brings up the DEEPX
 * 0.75 V rail in response to a rising edge on `DEEPX_PWR_EN_REQ`
 * (Renesas pad P65), then drives `DEEPX_CORE_0P75_EN` (P64) high
 * to release the rest of the DEEPX clamps.
 *
 * Wiring (see `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`):
 *
 *   P65  IN   `DEEPX_PWR_EN_REQ`     -- rising edge fires this module.
 *   P64  OUT  `DEEPX_CORE_0P75_EN`   -- driven high after CH2 PG.
 *
 * DA9292 control rides the supervisor's BRD_I²C handle (the same
 * physical bus the GD32 bridge uses for management commands).  The
 * supervisor's mutex serialises the two consumers; this module
 * borrows the I²C handle via
 * `alp_z_v2n_supervisor_brd_i2c_acquire()` for the duration of the
 * DA9292 register sequence and releases as soon as the rail is
 * verified PG.  Total bus hold-time is bounded by the DA9292
 * soft-start (~5 ms typical).
 *
 * Compiled in only when both `CONFIG_ALP_SDK_V2N_POWER_MGMT=y` and
 * `CONFIG_ALP_SDK_V2N_SUPERVISOR=y`; the second gate matches the
 * supervisor singleton's compile-time visibility because the I²C
 * handle this module borrows lives there.  Builds without the
 * supervisor see the stubs at the bottom of `v2n_power_mgmt.c`
 * and the module is a no-op.
 *
 * This header is SDK-internal.  Application code does not call
 * the API directly -- the supervisor's SYS_INIT hook attaches
 * the IRQ during boot.
 */

#ifndef ALP_INTERNAL_ZEPHYR_V2N_POWER_MGMT_H_
#define ALP_INTERNAL_ZEPHYR_V2N_POWER_MGMT_H_

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the DEEPX rail-bringup auxiliary.
 *
 * Configures P64 + P65 from devicetree, attaches the rising-edge
 * IRQ on P65, runs a `da9292_v2n_base_init` against the supervisor's
 * BRD_I²C handle so the DA9292 driver context is ready to receive
 * the eventual rail-up sequence.
 *
 * Idempotent across reinit (multiple SYS_INIT hooks on the same
 * build are not expected, but defensive coding here keeps the
 * supervisor invalidate/restore path clean).
 *
 * @return ALP_OK on success;
 *         ALP_ERR_NOSUPPORT if the supervisor is not compiled in
 *           or the V2N power-mgmt DT nodes are missing;
 *         ALP_ERR_NOT_READY if a GPIO controller could not bind
 *           or the DA9292 base-init failed (board ordering,
 *           PMIC not present);
 *         ALP_ERR_IO on a transport-level failure during the base
 *           init.
 */
alp_status_t alp_z_v2n_power_mgmt_init(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_INTERNAL_ZEPHYR_V2N_POWER_MGMT_H_ */
