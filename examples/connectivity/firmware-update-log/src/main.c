/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-update-log -- portable, tamper-evident update audit log.
 *
 * This is the application-facing API. The application code stays the same on
 * every SoM; only the backend changes. On native_sim this uses the software
 * backend. On hardware with a proven secure backend, the same calls can become
 * hardware-enforced.
 *
 * The basic flow is:
 *
 * - Open the log.
 * - Add one firmware-update record.
 * - Verify that nobody changed the history.
 * - Print the history.
 *
 * The software backend detects tampering. A secure hardware backend also keeps
 * the normal application from changing the protected log storage.
 *
 * Flow: open the log, append one update record (as MCUboot/a secure service
 * would after verifying an image), then verify the whole chain and print it.
 */
#include <stdio.h>
#include <string.h>

#include <alp/update_log.h>

/* Turn the assurance level into text. This tells the application what guarantee
 * it actually got on this build. */
static const char *assurance_str(alp_update_log_assurance_t a)
{
	return (a == ALP_UPDATE_LOG_HW_ENFORCED) ? "HW_ENFORCED (TF-M isolated)"
	                                         : "SW_TAMPER_EVIDENT (software tier)";
}

/* Turn the verify result into text. Verification checks the whole history, not
 * just the newest entry. */
static const char *verdict_str(alp_update_log_verdict_t v)
{
	switch (v) {
	case ALP_UPDATE_LOG_VERIFY_OK:
		/* The stored history is consistent. */
		return "OK";
	case ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN:
		/* Someone changed or moved an old entry. */
		return "CHAIN_BROKEN";
	case ALP_UPDATE_LOG_VERIFY_TRUNCATED:
		/* The end of the log is missing. */
		return "TRUNCATED";
	case ALP_UPDATE_LOG_VERIFY_ROLLED_BACK:
		/* The log was replaced with an older copy. */
		return "ROLLED_BACK";
	default:
		return "?";
	}
}

int main(void)
{
	/* Open the update log through the portable Alp API. Application code does
	 * not need to know which vendor-specific backend serves the log. */
	alp_update_log_t *log = alp_update_log_open();
	if (log == NULL) {
		/* No backend was built for this target. In a real product, this could
		 * be treated as a startup failure if the audit log is mandatory. */
		printf("[update-log] no backend present\n");
		return 0;
	}

	/* Print the guarantee level. HW_ENFORCED is reported only when the secure
	 * backend is present and ready. */
	printf("[update-log] assurance: %s\n", assurance_str(alp_update_log_assurance(log)));

	/* Build one example audit record. In a product, the bootloader or secure
	 * boot code should provide these values after it verifies the image. This
	 * example fills them by hand so the record shape is visible. */
	alp_update_log_entry_t e;
	memset(&e, 0, sizeof(e));

	/* Firmware version we want to record. */
	strncpy(e.fw_version, "1.4.2", ALP_UPDATE_LOG_FWVER_MAX);

	/* Placeholder for the verified image hash. In a product, this should come
	 * from trusted boot metadata, not from the normal application. */
	memset(e.image_hash, 0x5A, sizeof(e.image_hash)); /* SHA-256 of the image */

	/* CONFIRMED means the new firmware booted and was accepted as good. */
	e.status    = ALP_UPDATE_STATUS_CONFIRMED;

	/* Timestamp is useful for audits, but the security claim does not depend
	 * on time unless the product has a trusted clock. */
	e.timestamp = 1718000000u; /* best-effort epoch */

	/* Append the record. The log assigns the sequence number, so the app cannot
	 * choose where the entry sits in history. */
	if (alp_update_log_append(log, &e) != ALP_OK) {
		printf("[update-log] append failed\n");
		return 0;
	}

	/* Verify the whole log. The software backend detects changes. A secure
	 * hardware backend should also prevent the normal application from making
	 * those changes. */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	if (alp_update_log_verify(log, &v, &bad) == ALP_OK) {
		/* bad would identify the first bad entry. This example prints only
		 * the simple happy path. */
		printf("[update-log] verify: %s\n", verdict_str(v));
	}

	/* Print the audit trail. A product could send the same information to a
	 * cloud service, service tool, or compliance report. */
	uint64_t n = 0;
	if (alp_update_log_count(log, &n) == ALP_OK) {
		printf("[update-log] %llu entr%s:\n", (unsigned long long)n, (n == 1) ? "y" : "ies");
		for (uint64_t i = 0; i < n; i++) {
			alp_update_log_entry_t r;
			if (alp_update_log_get(log, i, &r) == ALP_OK) {
				/* These are the fields an audit usually needs first:
				 * entry number, firmware version, result, and time. */
				printf("  #%llu  v=%s  status=%d  ts=%llu\n",
				       (unsigned long long)r.seq,
				       r.fw_version,
				       (int)r.status,
				       (unsigned long long)r.timestamp);
			}
		}
	}

	/* Close the log handle. */
	alp_update_log_close(log);
	return 0;
}
