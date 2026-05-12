/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/power.h>.  v0.5 ships the public surface
 * with INVAL-pre-checks + NOSUPPORT until the per-backend wiring lands:
 *
 *   - On Zephyr's portable `pm_policy_*` path the dispatch will
 *     forward to `pm_state_force` / `pm_policy_state_lock_*` once
 *     the per-SoC pm_state tables stabilise across the SoCs in
 *     scope (V2N / V2N-M1 / AEN-family).
 *   - On V2N the call routes through the GD32G553 supervisor's
 *     CMD_POWER_MODE_SET opcode (0x28).  The supervisor
 *     singleton must re-run its handshake on wakeup so the
 *     bridge stays usable -- that state-machine extension lands
 *     alongside the firmware HAL body.
 *
 * The single global handle pattern follows the brief: alp_power_t
 * is opaque + single-instance.  open() returns a pointer to the
 * static struct; close() is a no-op clear of the wake bitmap.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/power.h"
#include "handles.h"
#include "v2n_supervisor.h"

struct alp_power {
    bool     in_use;
    uint32_t wake_bitmap;
};

static struct alp_power g_alp_power;

alp_power_t *alp_power_open(void)
{
    alp_z_clear_last_error();
    g_alp_power.in_use      = true;
    g_alp_power.wake_bitmap = 0u;
    return &g_alp_power;
}

alp_status_t alp_power_configure_wake_source(alp_power_t *handle, uint32_t wake_bitmap)
{
    if (handle == NULL || !handle->in_use) return ALP_ERR_NOT_READY;
    handle->wake_bitmap = wake_bitmap;
    /* Backends gate on individual bits at sleep-request time so
     * configure() itself can succeed even when the active backend
     * doesn't yet wire any of the requested sources.  Misconfigured
     * setups surface at request_sleep() with ALP_ERR_NOSUPPORT. */
    return ALP_OK;
}

alp_status_t alp_power_request_sleep(alp_power_t *handle, alp_power_mode_t mode,
                                     uint32_t wake_after_ms, alp_power_wake_info_t *info)
{
    if (handle == NULL || !handle->in_use) return ALP_ERR_NOT_READY;
    /* RUN is never a valid sleep target -- the caller is asking to
     * stay awake, which is a no-op (and likely an API misuse). */
    if (mode == ALP_POWER_MODE_RUN) return ALP_ERR_INVAL;
    if (mode != ALP_POWER_MODE_SLEEP && mode != ALP_POWER_MODE_DEEP_SLEEP &&
        mode != ALP_POWER_MODE_STANDBY) {
        return ALP_ERR_INVAL;
    }
    /* No wake source AND no timer means the SoC would never wake.
     * Reject as INVAL so callers see the mistake. */
    if (handle->wake_bitmap == 0u && wake_after_ms == 0u) {
        return ALP_ERR_INVAL;
    }
    /* Reserved opcode at v0.5; firmware HAL body for
     * CMD_POWER_MODE_SET (0x28) is the gating dep.  Zephyr-side
     * pm_policy_* wiring also defers to a follow-up commit so the
     * surface stays portable across SoMs.  Return NOSUPPORT after
     * the INVAL pre-checks pass.
     *
     * Future bring-up (when the HAL body lands):
     *
     *   1. Push the configured wake_bitmap + wake_after_ms to the
     *      GD32 supervisor via the CMD_POWER_MODE_SET wire.
     *   2. Issue Zephyr's pm_state_force / pm_policy_state_lock_get
     *      so the local CPU enters the matching pm_state.
     *   3. On wakeup: clear the cached supervisor state via
     *      `alp_z_v2n_supervisor_invalidate()` so the next bridge
     *      call re-runs the GD32 handshake -- the chip may have
     *      reset across the sleep cycle.
     *
     * Today's NOSUPPORT path doesn't call invalidate (no actual
     * sleep happens, so no resume hook is needed), but the
     * invalidate symbol is exported so the wake handler can call
     * it without further plumbing once the HAL body lands. */
    if (info != NULL) {
        info->realised_mode = mode;
        info->wake_source   = 0u;
        info->slept_ms      = 0u;
    }
    return ALP_ERR_NOSUPPORT;
}

void alp_power_close(alp_power_t *handle)
{
    if (handle == NULL) return;
    handle->in_use      = false;
    handle->wake_bitmap = 0u;
}
