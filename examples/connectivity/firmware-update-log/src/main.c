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
 * is kept in protected storage.
 *
 * On AEN dual-M55 builds the hardware path is split by role: HP is the trusted
 * owner that writes the MRAM log, and HE is the application client that reaches
 * HP through a tiny MHU mailbox service. That split is HARDWARE-enforced by
 * blocking HE from writing the MRAM log partition directly. The block does NOT
 * come from the MRAM slave-side firewall (FC13, 0x80000000+): that range is
 * owned and opened to the application masters by the SERAM firmware and is
 * outside the OEM device config (Alif AUGD0005, "Open Firewall configuration").
 * It comes instead from the HE core's own MASTER-side firewall (FC8), which the
 * OEM ATOC device config CAN program: an allow-all region for every master plus
 * a higher-priority deny carve-out over the log window for HE's master id. With
 * that policy HE boots and runs normally but bus-faults on any direct write to
 * the log partition -- silicon-proven on E8 (the probe below reports RESULT_FAULT
 * at STAGE_WRITE and the MRAM stays unchanged). HE reports HW_ENFORCED once that
 * FC8 policy is provisioned (CONFIG_..._FIREWALL_PROVEN) and the HP owner answers.
 *
 * Flow: open the log, append one update record (as MCUboot/a secure service
 * would after verifying an image), then verify the whole chain and print it.
 */
#include <stdio.h>
#include <string.h>

#include <alp/update_log.h>

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE)
#include <cmsis_core.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#endif

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)
#include <alp/mproc.h>
#endif

#include <zephyr/storage/flash_map.h>

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_PERSIST) && PARTITION_EXISTS(alp_ulog_partition)
#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HE) || defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)
#define UPDATE_LOG_STORAGE_STR "MRAM NVS (alp_ulog_partition)"
#else
#define UPDATE_LOG_STORAGE_STR "persistent NVS (alp_ulog_partition)"
#endif
#else
#define UPDATE_LOG_STORAGE_STR "RAM fallback"
#endif

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)
#define HE_LOAD_ADDR 0x58000000U
void alp_update_log_aen_m55_owner_run(void);
#endif

#if !defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)
/* Turn the assurance level into text. This tells the application what guarantee
 * it actually got on this build. */
static const char *assurance_str(alp_update_log_assurance_t a)
{
	return (a == ALP_UPDATE_LOG_HW_ENFORCED) ? "HW_ENFORCED (secure tier)"
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
#endif

#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE)
#define FIREWALL_PROBE_BEACON       ((volatile uint32_t *)0x02001080U)
#define FIREWALL_PROBE_MAGIC        0x46575052U /* "FWPR" */
#define FIREWALL_PROBE_RESULT_START 1U
#define FIREWALL_PROBE_RESULT_FAULT 2U
#define FIREWALL_PROBE_RESULT_PASS  3U
#define FIREWALL_PROBE_RESULT_FAIL  4U
#define FIREWALL_PROBE_RESULT_ERROR 5U
#define FIREWALL_PROBE_RESULT_CHECK 6U

#define FIREWALL_PROBE_STAGE_IDLE       0U
#define FIREWALL_PROBE_STAGE_PREPARE    1U
#define FIREWALL_PROBE_STAGE_WRITE      2U
#define FIREWALL_PROBE_STAGE_READ_AFTER 3U

static volatile uint32_t g_firewall_probe_stage;
static uint32_t          g_firewall_probe_pattern[4];

static void firewall_probe_stamp(uint32_t result, uint32_t stage, uint32_t detail, uint32_t pc)
{
	FIREWALL_PROBE_BEACON[0] = FIREWALL_PROBE_MAGIC;
	FIREWALL_PROBE_BEACON[1] = result;
	FIREWALL_PROBE_BEACON[2] = stage;
	FIREWALL_PROBE_BEACON[3] = detail;
	FIREWALL_PROBE_BEACON[4] = pc;
	FIREWALL_PROBE_BEACON[5] = SCB->CFSR;
	FIREWALL_PROBE_BEACON[6] = SCB->BFAR;
	FIREWALL_PROBE_BEACON[7] = SCB->HFSR;
#if PARTITION_EXISTS(alp_ulog_partition)
	FIREWALL_PROBE_BEACON[8] = PARTITION_OFFSET(alp_ulog_partition);
#else
	FIREWALL_PROBE_BEACON[8] = 0xFFFFFFFFU;
#endif
	FIREWALL_PROBE_BEACON[9]  = g_firewall_probe_pattern[0];
	FIREWALL_PROBE_BEACON[10] = g_firewall_probe_pattern[1];
	FIREWALL_PROBE_BEACON[11] = g_firewall_probe_pattern[2];
	FIREWALL_PROBE_BEACON[12] = g_firewall_probe_pattern[3];
	__DSB();
	__ISB();
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	uint32_t stage = g_firewall_probe_stage;
	uint32_t pc    = (esf != NULL) ? esf->basic.pc : 0U;
	uint32_t result;

	if (stage == FIREWALL_PROBE_STAGE_WRITE) {
		result = FIREWALL_PROBE_RESULT_FAULT;
	} else if (stage == FIREWALL_PROBE_STAGE_READ_AFTER) {
		result = FIREWALL_PROBE_RESULT_CHECK;
	} else {
		result = FIREWALL_PROBE_RESULT_ERROR;
	}

	firewall_probe_stamp(result, stage, reason, pc);
	for (;;) {
		__WFE();
	}
}

static int run_firewall_probe(void)
{
#if !PARTITION_EXISTS(alp_ulog_partition)
	firewall_probe_stamp(FIREWALL_PROBE_RESULT_ERROR, FIREWALL_PROBE_STAGE_IDLE, 1U, 0U);
	printf("[update-log] firewall probe: alp_ulog_partition is missing\n");
	return 1;
#else
	const struct device *flash = PARTITION_DEVICE(alp_ulog_partition);
	const off_t          off   = PARTITION_OFFSET(alp_ulog_partition);
	uint32_t             after[4];

	firewall_probe_stamp(FIREWALL_PROBE_RESULT_START, FIREWALL_PROBE_STAGE_IDLE, 0U, 0U);
	printf("[update-log] HE direct-write firewall probe: MRAM offset=0x%08lx\n", (long)off);

	if (!device_is_ready(flash)) {
		firewall_probe_stamp(FIREWALL_PROBE_RESULT_ERROR, FIREWALL_PROBE_STAGE_IDLE, 2U, 0U);
		printf("[update-log] firewall probe: MRAM flash device is not ready\n");
		return 1;
	}

	g_firewall_probe_stage      = FIREWALL_PROBE_STAGE_PREPARE;
	uint32_t nonce              = k_cycle_get_32() ^ (uint32_t)off;
	g_firewall_probe_pattern[0] = 0x46575052U; /* "FWPR" */
	g_firewall_probe_pattern[1] = nonce ^ 0x13579BDFU;
	g_firewall_probe_pattern[2] = nonce ^ 0x2468ACE0U;
	g_firewall_probe_pattern[3] = 0x21574C41U; /* "ALW!" */
	g_firewall_probe_stage      = FIREWALL_PROBE_STAGE_IDLE;
	firewall_probe_stamp(FIREWALL_PROBE_RESULT_START, FIREWALL_PROBE_STAGE_IDLE, 0U, 0U);

	g_firewall_probe_stage = FIREWALL_PROBE_STAGE_WRITE;
	int rc = flash_write(flash, off, g_firewall_probe_pattern, sizeof(g_firewall_probe_pattern));
	g_firewall_probe_stage = FIREWALL_PROBE_STAGE_IDLE;

	g_firewall_probe_stage = FIREWALL_PROBE_STAGE_READ_AFTER;
	int read_rc            = flash_read(flash, off, after, sizeof(after));
	g_firewall_probe_stage = FIREWALL_PROBE_STAGE_IDLE;
	if (read_rc != 0) {
		uint32_t detail = (rc == 0) ? 0U : (uint32_t)-rc;
		firewall_probe_stamp(
		    FIREWALL_PROBE_RESULT_CHECK, FIREWALL_PROBE_STAGE_READ_AFTER, detail, 0U);
		printf("[update-log] firewall probe: HE post-read blocked rc=%d; compare by SWD\n",
		       read_rc);
		return 0;
	}

	if (memcmp(after, g_firewall_probe_pattern, sizeof(after)) == 0) {
		firewall_probe_stamp(FIREWALL_PROBE_RESULT_FAIL,
		                     FIREWALL_PROBE_STAGE_WRITE,
		                     (rc == 0) ? 0U : (uint32_t)-rc,
		                     0U);
		printf("[update-log] FAIL: HE direct MRAM write changed alp_ulog_partition\n");
		return 1;
	}

	firewall_probe_stamp(
	    FIREWALL_PROBE_RESULT_PASS, FIREWALL_PROBE_STAGE_WRITE, (rc == 0) ? 0U : (uint32_t)-rc, 0U);
	printf("[update-log] PASS: HE direct MRAM write pattern did not land");
	printf(" (rc=%d)\n", rc);
	return 0;
#endif
}
#endif

int main(void)
{
#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER)
	printf("[update-log] AEN HP owner: MRAM writer service starting\n");
	printf("[update-log] owner storage: %s\n", UPDATE_LOG_STORAGE_STR);

	/* HP releases HE after the package has loaded the HE image. From then on,
	 * HE can only append by sending an MHU request back to this owner. */
	alp_status_t rc = alp_mproc_boot_core(ALP_CORE_M55_HE, HE_LOAD_ADDR);
	printf("[update-log] released HE rc=%d\n", (int)rc);

	/* This call does not return. It opens the local MRAM/NVS log and serves
	 * HE append/verify/count/get requests. */
	alp_update_log_aen_m55_owner_run();
	return 0;
#else
#if defined(CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE)
	return run_firewall_probe();
#endif

	/* Open the device's update log. NULL means no backend on this SoM. */
	alp_update_log_t *log = alp_update_log_open();
	if (log == NULL) {
#if defined(CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED)
		printf("[update-log] HW_ENFORCED required, but no secure owner/firewall-backed backend is "
		       "active\n");
#else
		printf("[update-log] no backend present\n");
#endif
		return 0;
	}

	/* Print the actual guarantee. Do not assume HW_ENFORCED just because the
	 * chip has secure hardware; the backend reports only what this build and
	 * boot path really wired. */
	printf("[update-log] assurance: %s\n", assurance_str(alp_update_log_assurance(log)));
	printf("[update-log] storage: %s\n", UPDATE_LOG_STORAGE_STR);

	/* Record an update result. This example fills the fields by hand so the
	 * flow is easy to read. Production code should prefer
	 * alp_update_log_append_boot(), once the board provides authenticated boot
	 * metadata, so the version/hash/status come from verified boot facts
	 * instead of from normal application data. */
	alp_update_log_entry_t e;
	memset(&e, 0, sizeof(e));

	/* Firmware version we want to record. */
	strncpy(e.fw_version, "1.4.2", ALP_UPDATE_LOG_FWVER_MAX);

	/* Placeholder for the verified image hash. In a product, this should come
	 * from trusted boot metadata, not from the normal application. */
	memset(e.image_hash, 0x5A, sizeof(e.image_hash)); /* SHA-256 of the image */

	/* CONFIRMED means the new firmware booted and was accepted as good.
	 * Timestamp is useful for audits, but the security claim does not depend
	 * on time unless the product has a trusted clock. */
	e.status    = ALP_UPDATE_STATUS_CONFIRMED;
	e.timestamp = 1718000000u; /* best-effort epoch */

	/* Append the record. The log assigns the sequence number, so the app cannot
	 * choose where the entry sits in history. */
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
#endif
}
