/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software mproc fallback.  Wildcard backend at priority 0 --
 * picked only when no hardware backend is linked into the build
 * (native_sim trimmed-image case).  No real MHU / shared-memory
 * carve-out / hwsem block exists under native_sim, so the stub
 * lets examples that include <alp/mproc.h> compile and exercise
 * the dispatcher without pulling in CONFIG_MBOX or any DT alias.
 *
 * Contract: NOSUPPORT stub.  It registers (priority 0, "*") only so
 * the `mproc` class section is never empty -- that keeps the linker
 * emitting the registry's __start_/__stop_ bounds.  No MHU /
 * shared-memory / hwsem hardware exists under native_sim, so:
 *   - shmem_open / mbox_open / hwsem_open -> ALP_ERR_NOSUPPORT (the
 *     dispatcher relays this as a NULL handle + last_error)
 *   - all I/O ops (view / send / lock / unlock) keep their
 *     no-op / NOT_IMPLEMENTED bodies but are unreachable, since no
 *     handle is ever handed out
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~250 B, zero RAM (no per-handle backend state --
 *      every state->be_data is left NULL).
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_NOT_IMPLEMENTED with no Zephyr-subsystem
 *      touch.  hwsem_lock with a non-zero timeout still returns
 *      ALP_OK immediately -- the stub does NOT block.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>

#include "mproc_ops.h"

/* ---------- Shared memory ---------- */

static alp_status_t sw_shmem_open(const alp_shmem_config_t *cfg, alp_shmem_backend_state_t *state,
                                  alp_capabilities_t *caps_out)
{
	/* NOSUPPORT stub: no shared-memory carve-out on native_sim. */
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_shmem_view(alp_shmem_backend_state_t *state, void **base_out,
                                  size_t *size_out)
{
	(void)state;
	if (base_out != NULL) *base_out = NULL;
	if (size_out != NULL) *size_out = 0;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_shmem_close(alp_shmem_backend_state_t *state)
{
	(void)state;
}

/* ---------- Mailbox ---------- */

static alp_status_t sw_mbox_open(const alp_mbox_config_t *cfg, alp_mbox_backend_state_t *state,
                                 alp_capabilities_t *caps_out)
{
	/* NOSUPPORT stub: no MHU / mailbox device on native_sim. */
	(void)cfg;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_mbox_send(alp_mbox_backend_state_t *state, const void *data, size_t len,
                                 uint32_t timeout_ms)
{
	(void)state;
	(void)data;
	(void)len;
	(void)timeout_ms;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_mbox_set_callback(alp_mbox_backend_state_t *state, alp_mbox_msg_cb_t cb,
                                         void *user)
{
	(void)state;
	(void)cb;
	(void)user;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_mbox_close(alp_mbox_backend_state_t *state)
{
	(void)state;
}

/* ---------- Hardware semaphore ---------- */

static alp_status_t sw_hwsem_open(uint32_t hwsem_id, alp_hwsem_backend_state_t *state,
                                  alp_capabilities_t *caps_out)
{
	/* NOSUPPORT stub: no hardware semaphore block on native_sim. */
	(void)hwsem_id;
	(void)state;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_hwsem_try_lock(alp_hwsem_backend_state_t *state)
{
	(void)state;
	/* No-op lock under native_sim -- the stub returns ALP_OK so
     * tests that exercise the lock-protected code path can run
     * without a real cross-core sync primitive being available. */
	return ALP_OK;
}

static alp_status_t sw_hwsem_lock(alp_hwsem_backend_state_t *state, uint32_t timeout_ms)
{
	(void)state;
	(void)timeout_ms;
	/* Same no-op semantics as try_lock -- never blocks. */
	return ALP_OK;
}

static alp_status_t sw_hwsem_unlock(alp_hwsem_backend_state_t *state)
{
	(void)state;
	return ALP_OK;
}

static void sw_hwsem_close(alp_hwsem_backend_state_t *state)
{
	(void)state;
}

/* ---------- Registration ---------- */

static const alp_mproc_ops_t _ops = {
	.shmem_open        = sw_shmem_open,
	.shmem_view        = sw_shmem_view,
	.shmem_close       = sw_shmem_close,
	.mbox_open         = sw_mbox_open,
	.mbox_send         = sw_mbox_send,
	.mbox_set_callback = sw_mbox_set_callback,
	.mbox_close        = sw_mbox_close,
	.hwsem_open        = sw_hwsem_open,
	.hwsem_try_lock    = sw_hwsem_try_lock,
	.hwsem_lock        = sw_hwsem_lock,
	.hwsem_unlock      = sw_hwsem_unlock,
	.hwsem_close       = sw_hwsem_close,
};

ALP_BACKEND_REGISTER(mproc, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
