/* SPDX-License-Identifier: Apache-2.0  -- internal, NOT public. */
#ifndef ALP_BACKENDS_UPDATE_LOG_OPS_H
#define ALP_BACKENDS_UPDATE_LOG_OPS_H

#include <stdbool.h>
#include "alp/backend.h"
#include "alp/update_log.h"

typedef struct alp_update_log_ops {
	alp_update_log_assurance_t assurance;
	alp_status_t (*append)(const alp_update_log_entry_t *e);
	alp_status_t (*verify)(alp_update_log_verdict_t *v, uint64_t *bad);
	alp_status_t (*count)(uint64_t *out);
	alp_status_t (*get)(uint64_t seq, alp_update_log_entry_t *out);
} alp_update_log_ops_t;

/* Dispatcher-owned handle. Single instance (one log per device). */
struct alp_update_log {
	const alp_update_log_ops_t *ops;
	alp_update_log_assurance_t  assurance;
	bool                        in_use;
};

#endif
