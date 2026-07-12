/* SPDX-License-Identifier: Apache-2.0  -- internal, NOT public. */
#ifndef ALP_BACKENDS_UPDATE_LOG_OPS_H
#define ALP_BACKENDS_UPDATE_LOG_OPS_H

#include <stdbool.h>
#include "alp/backend.h"
#include "alp/update_log.h"

typedef struct alp_update_log_ops {
	alp_update_log_assurance_t assurance;
	/* Open-time readiness probe (optional; NULL == always ready). The
	 * dispatcher calls this once per alp_update_log_open() and, on
	 * ALP_ERR_NOSUPPORT, falls through to the next-ranked backend
	 * (issue #239 pattern). It lets a higher-priority tier that cannot
	 * serve on this boot -- e.g. the HW_ENFORCED TF-M tier when its secure
	 * owner is not present or not ready --
	 * decline cleanly so the SW tamper-evident tier serves instead,
	 * rather than binding a backend whose append() would just fail. Any
	 * status other than ALP_OK / ALP_ERR_NOSUPPORT is a hard error and
	 * is surfaced, not masked by a lower tier. */
	alp_status_t (*ready)(void);
	alp_status_t (*append)(const alp_update_log_entry_t *e);
	alp_status_t (*verify)(alp_update_log_verdict_t *v, uint64_t *bad);
	alp_status_t (*count)(uint64_t *out);
	alp_status_t (*get)(uint64_t seq, alp_update_log_entry_t *out);
} alp_update_log_ops_t;

/* Dispatcher-owned handle. Single instance (one log per device). */
struct alp_update_log {
	const alp_update_log_ops_t *ops;
	alp_update_log_assurance_t  assurance;
	/* lifecycle/active_ops drive the race-safe one-time init + op/close
	 * guard in src/update_log_dispatch.c (issue #629). Unlike the pooled
	 * classes this is a singleton with an idempotent open(), so the
	 * lifecycle byte also carries a dispatcher-local OPENING state that
	 * elects exactly one initializer among racing first-opens. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#ifdef CONFIG_ZTEST
/* Test-only reboot emulation for the sw tier: drops all of the backend's
 * RAM state (RAM store, RAM counter, cached NVS mount). wipe=true also
 * clears the NVS partition. Compiled only under ZTEST. */
void alp_ulog_sw_tier_test_reset(bool wipe);
#endif

#endif
