/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-se-service-query -- exercise the READ-ONLY Secure-Enclave (SE) service
 * surface on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run +
 * RAM-console flow.  The companion to aen-se-service-info (which proved the
 * transport binds + the single LCS read); this app dumps the broader set of
 * ZERO-RISK SE queries the hal_alif client exposes, to characterise what the SE
 * reports on a maker-provisioned E8.
 *
 * SAFETY -- ONLY READ-ONLY / NON-MUTATING SE SERVICES ARE CALLED HERE:
 *   se_service_heartbeat            -- liveness ping
 *   se_service_get_se_revision      -- SE firmware revision string
 *   se_service_get_toc_number       -- number of TOC entries
 *   se_service_get_toc_version      -- TOC version
 *   se_service_get_device_part_number
 *   se_service_system_get_device_data -- LCS + revision + part/serial (the #192 read)
 *   se_service_get_run_cfg          -- current run power/clock profile
 *   se_service_get_off_cfg          -- off/standby power/wake profile
 *   se_service_get_rnd_num          -- TRNG sanity (8 bytes)
 *
 * DELIBERATELY NOT CALLED (mutating / destructive -- a wrong value can brown out
 * the rail, change the security posture, or BRICK secure boot; these need a
 * recovery plan + explicit sign-off before any bench run):
 *   se_service_update_stoc          -- REWRITES the secure-boot TOC in MRAM
 *   se_service_set_run_cfg / set_off_cfg / clock_set_divider -- power/clock change
 *   se_service_boot_es0 / shutdown_es0 / boot_reset_soc / boot_reset_cpu
 *   se_service_se_sleep_req / system_set_services_debug
 *
 * Every call below bounds its wait inside se_service.c (returns 0 / -EAGAIN /
 * -EBUSY / a positive SE error), so the app never hangs.  PASS gate: the SE
 * answers every read-only query rc=0.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>

/* hal_alif SE service client (Apache-2.0). */
#include <se_service.h>

/* SE firmware revision string buffer (VERSION_RESPONSE_LENGTH = 80). */
#define SE_REV_LEN 80

/* LCS legend (Alif SE service reference). */
static const char *lcs_name(uint8_t lcs)
{
	switch (lcs) {
	case 0x0U:
		return "CM (chip manufacturer)";
	case 0x1U:
		return "DM (device manufacturer)";
	case 0x5U:
		return "SE (secure-enabled)";
	case 0x7U:
		return "RMA";
	default:
		return "<unknown>";
	}
}

static void hexdump(const char *label, const uint8_t *p, size_t n)
{
	printk("        %s = ", label);
	for (size_t i = 0U; i < n; i++) {
		printk("%02x", p[i]);
	}
	printk("\n");
}

int main(void)
{
	int  rc;
	bool all_ok = true;

	printk("\n=== aen-se-service-query (read-only SE surface) ===\n");

	/* 0. Liveness: a heartbeat round-trip proves the SE answers the mailboxes. */
	rc = se_service_heartbeat();
	printk("heartbeat            : rc=%d\n", rc);
	all_ok &= (rc == 0);

	/* 1. SE firmware revision string. */
	uint8_t se_rev[SE_REV_LEN];

	memset(se_rev, 0, sizeof(se_rev));
	rc                     = se_service_get_se_revision(se_rev);
	se_rev[SE_REV_LEN - 1] = '\0';
	printk("se_revision          : rc=%d \"%s\"\n", rc, (const char *)se_rev);
	all_ok &= (rc == 0);

	/* 2. TOC count + version. */
	uint32_t toc_num = 0U, toc_ver = 0U;

	rc = se_service_get_toc_number(&toc_num);
	printk("toc_number           : rc=%d n=%u\n", rc, toc_num);
	all_ok &= (rc == 0);

	rc = se_service_get_toc_version(&toc_ver);
	printk("toc_version          : rc=%d 0x%08x\n", rc, toc_ver);
	all_ok &= (rc == 0);

	/* 3. Device part number. */
	uint32_t part = 0U;

	rc = se_service_get_device_part_number(&part);
	printk("device_part_number   : rc=%d 0x%08x\n", rc, part);
	all_ok &= (rc == 0);

	/* 4. Full device data: LCS + revision + part + serial (the #192 read, expanded). */
	get_device_revision_data_t dev = { 0 };

	rc = se_service_system_get_device_data(&dev);
	printk("device_data          : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		printk("        revision_id = 0x%08x\n", (uint32_t)dev.revision_id);
		printk("        LCS         = 0x%02x  (%s)\n", (unsigned int)dev.LCS, lcs_name(dev.LCS));
		hexdump("ALIF_PN    ", (const uint8_t *)dev.ALIF_PN, sizeof(dev.ALIF_PN));
		hexdump("SerialN    ", (const uint8_t *)dev.SerialN, sizeof(dev.SerialN));
	}

	/* 5. Current RUN power/clock profile (read-only -- set_run_cfg is NOT called). */
	run_profile_t run = { 0 };

	rc = se_service_get_run_cfg(&run);
	printk("run_cfg              : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		printk("        dcdc_voltage=%u mV  power_domains=0x%08x  memory_blocks=0x%08x\n",
		       run.dcdc_voltage,
		       run.power_domains,
		       run.memory_blocks);
		printk("        run_clk_src=%d cpu_clk_freq=%d aon_clk_src=%d vdd_ioflex_3V3=%d\n",
		       (int)run.run_clk_src,
		       (int)run.cpu_clk_freq,
		       (int)run.aon_clk_src,
		       (int)run.vdd_ioflex_3V3);
	}

	/* 6. OFF / standby profile (read-only -- set_off_cfg is NOT called). */
	off_profile_t off = { 0 };

	rc = se_service_get_off_cfg(&off);
	printk("off_cfg              : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		printk("        dcdc_voltage=%u mV  wakeup_events=0x%08x  ewic_cfg=0x%08x\n",
		       off.dcdc_voltage,
		       off.wakeup_events,
		       off.ewic_cfg);
		printk("        vtor=0x%08x vtor_ns=0x%08x\n", off.vtor_address, off.vtor_address_ns);
	}

	/* 7. TRNG sanity: pull 8 bytes from the SE entropy source. */
	uint8_t rnd[8] = { 0 };

	rc = se_service_get_rnd_num(rnd, sizeof(rnd));
	printk("get_rnd_num(8)       : rc=%d\n", rc);
	all_ok &= (rc == 0);
	if (rc == 0) {
		hexdump("rnd        ", rnd, sizeof(rnd));
	}

	if (all_ok) {
		printk("RESULT PASS: SE answered every read-only query (heartbeat + se_revision "
		       "+ toc + part + device_data[LCS=0x%02x] + run_cfg[%umV] + off_cfg + rnd) "
		       "-- no mutating service called (update_stoc/set_run_cfg gated out)\n",
		       (unsigned int)dev.LCS,
		       run.dcdc_voltage);
	} else {
		printk("RESULT FAIL: at least one read-only SE query did not return rc=0 "
		       "(SE asleep/unreachable on this boot, or a service unsupported) -- see "
		       "the per-call rc above\n");
	}

	return 0;
}
