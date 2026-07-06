/* SPDX-License-Identifier: Apache-2.0
 *
 * TF-M PSA Protected Storage owner for the update-log hardware tier.
 *
 * This file is the secure-side implementation behind the client backend in
 * src/backends/update_log/tfm_psa.c. It must run as the secure owner of the log
 * state. On Alif E4/E8 that means a secure M55/TF-M image whose storage region
 * is protected by the Alif SE/firewall policy, with the non-secure app reaching
 * it only through the narrow append/verify/count/get service seam.
 */

#include "alp/update_log.h"

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_TFM_PSA_OWNER)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <psa/protected_storage.h>
#include <psa/storage_common.h>

#include "update_log/engine.h"
#include "update_log/secure_service.h"
#include "update_log/store.h"

/*
 * PSA UIDs are intentionally kept below Zephyr's 30-bit default UID ceiling.
 * The names mirror the engine keys:
 *   ulog.meta  -> metadata asset
 *   ulog.N     -> write-once entry asset at ENTRY_BASE + N
 *   counter    -> secure monotonic high-watermark asset
 */
#define ULOG_UID_META       ((psa_storage_uid_t)0x00504100u)
#define ULOG_UID_COUNTER    ((psa_storage_uid_t)0x00504101u)
#define ULOG_UID_ENTRY_BASE ((psa_storage_uid_t)0x00504200u)
#define ULOG_UID_MAX_30BIT  UINT64_C(0x3fffffff)

static alp_status_t psa_to_alp(psa_status_t st)
{
	switch (st) {
	case PSA_SUCCESS:
		return ALP_OK;
	case PSA_ERROR_INVALID_ARGUMENT:
		return ALP_ERR_INVAL;
	case PSA_ERROR_DOES_NOT_EXIST:
		return ALP_ERR_NOT_FOUND;
	case PSA_ERROR_INSUFFICIENT_STORAGE:
		return ALP_ERR_NOMEM;
	case PSA_ERROR_NOT_SUPPORTED:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t parse_entry_key(const char *key, uint64_t *seq_out)
{
	if (key == NULL || seq_out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (strncmp(key, "ulog.", 5u) != 0 || key[5] == '\0') {
		return ALP_ERR_INVAL;
	}

	uint64_t seq = 0u;
	for (const char *p = key + 5; *p != '\0'; ++p) {
		if (*p < '0' || *p > '9') {
			return ALP_ERR_INVAL;
		}
		uint64_t digit = (uint64_t)(*p - '0');
		if (seq > (UINT64_MAX - digit) / 10u) {
			return ALP_ERR_NOMEM;
		}
		seq = (seq * 10u) + digit;
	}

	*seq_out = seq;
	return ALP_OK;
}

static alp_status_t uid_for_key(const char *key, psa_storage_uid_t *uid_out, bool *entry_out)
{
	if (key == NULL || uid_out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (strcmp(key, "ulog.meta") == 0) {
		*uid_out = ULOG_UID_META;
		if (entry_out != NULL) {
			*entry_out = false;
		}
		return ALP_OK;
	}

	uint64_t     seq = 0u;
	alp_status_t rc  = parse_entry_key(key, &seq);
	if (rc != ALP_OK) {
		return rc;
	}
	if (seq > ULOG_UID_MAX_30BIT - (uint64_t)ULOG_UID_ENTRY_BASE) {
		return ALP_ERR_NOMEM;
	}

	*uid_out = (psa_storage_uid_t)((uint64_t)ULOG_UID_ENTRY_BASE + seq);
	if (entry_out != NULL) {
		*entry_out = true;
	}
	return ALP_OK;
}

static alp_status_t ps_put(void *ctx, const char *key, const uint8_t *buf, size_t len)
{
	(void)ctx;
	if (buf == NULL) {
		return ALP_ERR_INVAL;
	}

	psa_storage_uid_t uid      = 0u;
	bool              is_entry = false;
	alp_status_t      rc       = uid_for_key(key, &uid, &is_entry);
	if (rc != ALP_OK) {
		return rc;
	}

	psa_storage_create_flags_t flags =
	    is_entry ? PSA_STORAGE_FLAG_WRITE_ONCE : PSA_STORAGE_FLAG_NONE;
	return psa_to_alp(psa_ps_set(uid, len, buf, flags));
}

static alp_status_t ps_get(void *ctx, const char *key, uint8_t *buf, size_t cap, size_t *out_len)
{
	(void)ctx;
	if (buf == NULL) {
		return ALP_ERR_INVAL;
	}

	psa_storage_uid_t uid = 0u;
	alp_status_t      rc  = uid_for_key(key, &uid, NULL);
	if (rc != ALP_OK) {
		return rc;
	}

	struct psa_storage_info_t info;
	psa_status_t              st = psa_ps_get_info(uid, &info);
	if (st != PSA_SUCCESS) {
		return psa_to_alp(st);
	}
	if (info.size > cap) {
		return ALP_ERR_NOMEM;
	}

	size_t got = 0u;
	st         = psa_ps_get(uid, 0u, info.size, buf, &got);
	if (st != PSA_SUCCESS) {
		return psa_to_alp(st);
	}
	if (out_len != NULL) {
		*out_len = got;
	}
	return ALP_OK;
}

static alp_status_t ps_erase(void *ctx, const char *key)
{
	(void)ctx;

	psa_storage_uid_t uid = 0u;
	alp_status_t      rc  = uid_for_key(key, &uid, NULL);
	if (rc != ALP_OK) {
		return rc;
	}
	return psa_to_alp(psa_ps_remove(uid));
}

static alp_status_t ps_counter_store(uint64_t value)
{
	return psa_to_alp(psa_ps_set(ULOG_UID_COUNTER, sizeof(value), &value, PSA_STORAGE_FLAG_NONE));
}

static alp_status_t ps_counter_read(void *ctx, uint32_t id, uint64_t *out)
{
	(void)ctx;
	if (id != 0u || out == NULL) {
		return ALP_ERR_INVAL;
	}

	size_t       got = 0u;
	psa_status_t st  = psa_ps_get(ULOG_UID_COUNTER, 0u, sizeof(*out), out, &got);
	if (st == PSA_ERROR_DOES_NOT_EXIST) {
		*out = 0u;
		return ps_counter_store(0u);
	}
	if (st != PSA_SUCCESS) {
		return psa_to_alp(st);
	}
	if (got != sizeof(*out)) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

static alp_status_t ps_counter_increment(void *ctx, uint32_t id, uint64_t *out)
{
	uint64_t     value = 0u;
	alp_status_t rc    = ps_counter_read(ctx, id, &value);
	if (rc != ALP_OK) {
		return rc;
	}
	if (value == UINT64_MAX) {
		return ALP_ERR_NOMEM;
	}

	++value;
	rc = ps_counter_store(value);
	if (rc != ALP_OK) {
		return rc;
	}
	if (out != NULL) {
		*out = value;
	}
	return ALP_OK;
}

static const alp_secure_store_if g_ps_store = {
	.put   = ps_put,
	.get   = ps_get,
	.erase = ps_erase,
	.ctx   = NULL,
};

static const alp_monotonic_counter_if g_ps_counter = {
	.read      = ps_counter_read,
	.increment = ps_counter_increment,
	.ctx       = NULL,
};

alp_status_t alp_update_log_secure_ready(void)
{
	uint64_t counter = 0u;

	return ps_counter_read(NULL, 0u, &counter);
}

alp_status_t alp_update_log_secure_append(const alp_update_log_entry_t *entry)
{
	return ulog_engine_append(&g_ps_store, &g_ps_counter, entry);
}

alp_status_t alp_update_log_secure_verify(alp_update_log_verdict_t *verdict, uint64_t *bad_seq)
{
	return ulog_engine_verify(&g_ps_store, &g_ps_counter, verdict, bad_seq);
}

alp_status_t alp_update_log_secure_count(uint64_t *out)
{
	return ulog_engine_count(&g_ps_store, &g_ps_counter, out);
}

alp_status_t alp_update_log_secure_get(uint64_t seq, alp_update_log_entry_t *out)
{
	return ulog_engine_get(&g_ps_store, seq, out);
}

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_TFM_PSA_OWNER */
