/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-update-log -- portable, tamper-evident update audit log.
 *
 * This is the ONE API a product uses regardless of SoM. On native_sim and on
 * boards without a secure storage backend, the software tier builds a hash
 * chain and verifies that nobody changed, removed, rolled back, or reordered
 * old records. That is tamper-evident, not app-immutable.
 *
 * On a TF-M build with a real secure owner, the same API can bind to
 * HW_ENFORCED: each entry is a secure-world write-once asset, and the counter
 * is kept in protected storage. On Alif E4/E8 this storage must be backed by
 * the SE/firewall policy, so normal application firmware cannot rewrite
 * history.
 *
 * Flow: open the log, append one update record (as MCUboot/a secure service
 * would after verifying an image), then verify the whole chain and print it.
 */
#include <stdio.h>
#include <string.h>

#include <alp/update_log.h>

static const char *assurance_str(alp_update_log_assurance_t a)
{
	return (a == ALP_UPDATE_LOG_HW_ENFORCED) ? "HW_ENFORCED (secure tier)"
	                                         : "SW_TAMPER_EVIDENT (software tier)";
}

static const char *verdict_str(alp_update_log_verdict_t v)
{
	switch (v) {
	case ALP_UPDATE_LOG_VERIFY_OK:
		return "OK";
	case ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN:
		return "CHAIN_BROKEN";
	case ALP_UPDATE_LOG_VERIFY_TRUNCATED:
		return "TRUNCATED";
	case ALP_UPDATE_LOG_VERIFY_ROLLED_BACK:
		return "ROLLED_BACK";
	default:
		return "?";
	}
}

int main(void)
{
	/* Open the device's update log. NULL means no backend on this SoM. */
	alp_update_log_t *log = alp_update_log_open();
	if (log == NULL) {
		printf("[update-log] no backend present\n");
		return 0;
	}

	/* Print the actual guarantee. Do not assume HW_ENFORCED just because the
	 * chip has secure hardware; the backend reports only what this build and
	 * boot path really wired. */
	printf("[update-log] assurance: %s\n", assurance_str(alp_update_log_assurance(log)));

	/* Record an update result. This example fills the fields by hand so the
	 * flow is easy to read. Production code should prefer
	 * alp_update_log_append_boot(), once the board provides authenticated boot
	 * metadata, so the version/hash/status come from verified boot facts
	 * instead of from normal application data. */
	alp_update_log_entry_t e;
	memset(&e, 0, sizeof(e));
	strncpy(e.fw_version, "1.4.2", ALP_UPDATE_LOG_FWVER_MAX);
	memset(e.image_hash, 0x5A, sizeof(e.image_hash)); /* SHA-256 of the image */
	e.status    = ALP_UPDATE_STATUS_CONFIRMED;
	e.timestamp = 1718000000u; /* best-effort epoch */

	if (alp_update_log_append(log, &e) != ALP_OK) {
		printf("[update-log] append failed\n");
		return 0;
	}

	/* Verify the whole chain. On the software tier this detects a broken
	 * history, but it cannot stop privileged firmware from rebuilding both the
	 * log and its counter. On the hardware tier, old entries live behind the
	 * secure boundary, so normal firmware cannot rewrite them. */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	if (alp_update_log_verify(log, &v, &bad) == ALP_OK) {
		printf("[update-log] verify: %s\n", verdict_str(v));
	}

	/* Print the append-only trail. */
	uint64_t n = 0;
	if (alp_update_log_count(log, &n) == ALP_OK) {
		printf("[update-log] %llu entr%s:\n", (unsigned long long)n, (n == 1) ? "y" : "ies");
		for (uint64_t i = 0; i < n; i++) {
			alp_update_log_entry_t r;
			if (alp_update_log_get(log, i, &r) == ALP_OK) {
				printf("  #%llu  v=%s  status=%d  ts=%llu\n",
				       (unsigned long long)r.seq,
				       r.fw_version,
				       (int)r.status,
				       (unsigned long long)r.timestamp);
			}
		}
	}

	alp_update_log_close(log);
	return 0;
}
