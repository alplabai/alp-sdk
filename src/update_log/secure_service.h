/* SPDX-License-Identifier: Apache-2.0
 * Internal secure-service seam for the update-log hardware tier. */
#ifndef ALP_UPDATE_LOG_SECURE_SERVICE_H
#define ALP_UPDATE_LOG_SECURE_SERVICE_H

#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/update_log.h"

/*
 * These functions are implemented by the secure owner of the log. In a
 * production TF-M build they should be veneers into a secure partition or
 * strong symbols compiled into the Secure Processing Environment. Weak
 * defaults in the non-secure backend return NOSUPPORT so the dispatcher can
 * fall through to the software tier when the secure owner is absent.
 */
alp_status_t alp_update_log_secure_ready(void);
alp_status_t alp_update_log_secure_append(const alp_update_log_entry_t *entry);
alp_status_t alp_update_log_secure_verify(alp_update_log_verdict_t *verdict, uint64_t *bad_seq);
alp_status_t alp_update_log_secure_count(uint64_t *out);
alp_status_t alp_update_log_secure_get(uint64_t seq, alp_update_log_entry_t *out);

#endif /* ALP_UPDATE_LOG_SECURE_SERVICE_H */
