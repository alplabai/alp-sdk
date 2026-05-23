/**
 * @file ext/renesas/power.h
 * @brief Renesas RZ/V2N power vendor-specific knobs.
 *
 * Non-portable.  Include only when you've committed to Renesas
 * V2N silicon for the gated feature.  Every function in this
 * header verifies the handle's backend is Renesas before
 * touching hardware; calls on a non-Renesas handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      First Renesas vendor-extension header.  Promotes to
 *      [ABI-STABLE] when three vendor families ship extensions.
 */

#ifndef ALP_EXT_RENESAS_POWER_H
#define ALP_EXT_RENESAS_POWER_H

#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/power.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_RENESAS_POWER_AVAILABLE 1

/** Supervisor-mode byte sent verbatim over CMD_POWER_MODE_SET.
 *
 *  Values 0..3 mirror @ref alp_power_mode_t; values 4..255 are the
 *  supervisor-firmware extension space (see the function-level doc on
 *  @ref alp_renesas_power_supervisor_mode_set for the mapping).  The
 *  typedef exists to make the intent explicit at the call site -- the
 *  raw uint8_t carries no hint that 4+ is reserved firmware-side. */
typedef uint8_t alp_renesas_power_supervisor_mode_t;

/**
 * @brief Send a low-level CMD_POWER_MODE_SET to the GD32G553 supervisor.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * V2N's M33 has no direct PMU; system-wide low-power transitions
 * are owned by the GD32G553 supervisor MCU, which gates rail
 * sequencing across the Renesas SoC.  The portable
 * @ref alp_power_request_sleep already routes through the supervisor
 * via CMD_POWER_MODE_SET (opcode 0x28) on V2N, but the portable
 * surface only carries the four logical modes from
 * @ref alp_power_mode_t.  This vendor surface lets callers issue a
 * supervisor-firmware-defined mode byte directly, for cases that
 * need to address a custom mode the supervisor exposes outside the
 * portable enum (e.g. a stretched DEEPX rail-down sequence on
 * specific board revisions).
 *
 * The @p supervisor_mode byte is sent verbatim as the @c mode field
 * of the wire payload defined in `docs/gd32-bridge-protocol.md` §3
 * for opcode 0x28.  The supervisor's interpretation:
 *
 *   - 0  = run (no-op; supervisor returns immediately)
 *   - 1  = sleep        (mirrors @ref ALP_POWER_MODE_SLEEP)
 *   - 2  = deep-sleep   (mirrors @ref ALP_POWER_MODE_DEEP_SLEEP)
 *   - 3  = standby      (mirrors @ref ALP_POWER_MODE_STANDBY)
 *   - 4+ = supervisor-firmware extension space; check
 *          `firmware/gd32-bridge/src/protocol.h` against the
 *          firmware build the bridge is currently running.  TBD
 *          values are silently rejected on the firmware side
 *          with STATUS_NOSUPPORT.
 *
 * The active wake-source bitmap mirrored in the handle (via the
 * preceding @ref alp_power_configure_wake_source) is forwarded
 * verbatim.  The call blocks the calling thread until the
 * supervisor returns (the supervisor manages the host-side wake
 * + bridge handshake re-init internally; the portable
 * @ref alp_power_request_sleep wraps the same machinery and is
 * the right call for most consumers).
 *
 * @par Supported silicon: renesas:rzv2n:n44 (other Renesas V2N SoCs
 * that ship the GD32G553 supervisor adopt this surface as they
 * gain the corresponding ALP_SOC_RENESAS_RZV2N_* gate).
 *
 * @param handle           Handle from @ref alp_power_open opened
 *                         against a Renesas V2N SoC.  Non-NULL.
 * @param supervisor_mode  Mode byte sent verbatim to the supervisor.
 *
 * @return @ref ALP_OK on success;
 *         @ref ALP_ERR_INVAL on NULL handle;
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC on non-Renesas backend;
 *         @ref ALP_ERR_NOSUPPORT if the supervisor firmware does
 *           not implement the requested mode;
 *         any error from the GD32 bridge transport.
 */
alp_status_t alp_renesas_power_supervisor_mode_set(
    alp_power_t                         *handle,
    alp_renesas_power_supervisor_mode_t  supervisor_mode);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_RENESAS_POWER_H */
