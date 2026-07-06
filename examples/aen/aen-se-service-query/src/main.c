/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-se-service-query -- exercise the READ-ONLY portable surfaces that the
 * Secure Enclave (SE) services back on the E1M-AEN801 (Ensemble E8, M55-HE),
 * via the bench RAM-run + RAM-console flow.  The companion to
 * aen-se-service-info (the vendor-specific transport bring-up regcheck): where
 * that app proves the SE MAILBOXES bind, this one shows what CUSTOMER CODE
 * calls once they do -- and it carries no vendor include.
 *
 * Portable surfaces exercised (all read-only / non-mutating):
 *   alp_soc_secure_fw_ping   (<alp/hw_info.h>)  -- controller liveness
 *   alp_soc_info_read        (<alp/hw_info.h>)  -- SoC identity: secure-fw
 *                             version string, part number, die revision,
 *                             lifecycle state, factory-fused serial
 *   alp_power_profile_get    (<alp/power.h>)    -- RUN + STANDBY operating
 *                             points (Hz / mV / opaque masks)
 *   alp_random_bytes         (<alp/security.h>) -- TRNG sanity (8 bytes; on
 *                             the E8 the security dispatcher routes this into
 *                             the SE CryptoCell TRNG)
 *
 * On the E8 the registry backends behind these surfaces ride the bench-proven
 * SE-service mailbox; every round-trip is bounded, so the app never hangs.
 * The mutating SE side (secure-boot TOC update, run/off profile writes, boot /
 * reset / sleep requests) is exactly what the portable surface does NOT map
 * here -- alp_power_profile_set exists but is deliberately never invoked (a
 * wrong value can brown out the rail; see the <alp/power.h> warning).
 *
 * Vendor-specific reads the previous revision dumped raw (the secure-boot TOC
 * entry count/version) are Alif bring-up detail with no portable meaning; the
 * vendor-scoped aen-se-service-info regcheck is where SE-transport poking
 * belongs.  The lifecycle state (LCS) survives portably as the
 * implementation-defined alp_soc_info_t::lifecycle code (E8 legend, from the
 * Alif SE service reference: 0x0 CM, 0x1 DM, 0x5 secure-enabled, 0x7 RMA).
 *
 * PASS gate: every portable call returns ALP_OK.  On builds without the SE
 * backends (native_sim) the same code links; the identity/profile calls
 * report ALP_ERR_NOSUPPORT and soc_ref still names the build's silicon.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/hw_info.h>
#include <alp/power.h>
#include <alp/security.h>

/* E8 lifecycle legend (Alif SE service reference).  The portable field is an
 * implementation-defined code; this decode is REPORTING sugar for the bench
 * log, keyed on the build's silicon -- not portable API. */
static const char *lifecycle_name(uint32_t lcs)
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
	alp_status_t rc;
	bool         all_ok = true;

	printk("\n=== aen-se-service-query (portable read-only surface) ===\n");

	/* 0. Liveness: a bounded ping proves the controller answers before we
	 *    trust the identity + profile reads. */
	rc = alp_soc_secure_fw_ping();
	printk("secure_fw_ping       : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);

	/* 1. SoC identity: soc_ref is build-time truth (filled even on
	 *    failure); the runtime fields come from the controller. */
	alp_soc_info_t info;

	rc = alp_soc_info_read(&info);
	printk("soc_info_read        : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	printk("        soc_ref     = \"%s\"\n", info.soc_ref);
	if (rc == ALP_OK) {
		printk("        secure_fw   = \"%s\"\n", info.secure_fw_version);
		printk("        part_number = 0x%08x\n", info.part_number);
		printk("        revision_id = 0x%08x\n", info.revision_id);
		printk(
		    "        lifecycle   = 0x%02x  (%s)\n", info.lifecycle, lifecycle_name(info.lifecycle));
		hexdump("serial     ", info.serial, info.serial_len);
	}

	/* 2. RUN operating-point profile (read-only -- profile_set is NOT
	 *    called). */
	alp_power_profile_t run = { 0 };

	rc = alp_power_profile_get(ALP_POWER_PROFILE_RUN, &run);
	printk("profile_get(RUN)     : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	if (rc == ALP_OK) {
		printk("        cpu=%u Hz  rail=%u mV  io=%u mV\n", run.cpu_clk_hz, run.rail_mv, run.io_mv);
		printk("        domains=0x%08x  memories=0x%08x\n", run.power_domains, run.memory_blocks);
	}

	/* 3. STANDBY operating-point profile (read-only). */
	alp_power_profile_t stby = { 0 };

	rc = alp_power_profile_get(ALP_POWER_PROFILE_STANDBY, &stby);
	printk("profile_get(STANDBY) : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	if (rc == ALP_OK) {
		printk(
		    "        cpu=%u Hz  rail=%u mV  io=%u mV\n", stby.cpu_clk_hz, stby.rail_mv, stby.io_mv);
		printk("        domains=0x%08x  memories=0x%08x  wake=0x%08x\n",
		       stby.power_domains,
		       stby.memory_blocks,
		       stby.wake_events);
	}

	/* 4. TRNG sanity: 8 random bytes through the portable security
	 *    surface (SE CryptoCell on the E8, PSA elsewhere). */
	uint8_t rnd[8] = { 0 };

	rc = alp_random_bytes(rnd, sizeof(rnd));
	printk("random_bytes(8)      : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	if (rc == ALP_OK) {
		hexdump("rnd        ", rnd, sizeof(rnd));
	}

	if (all_ok) {
		printk("RESULT PASS: every portable read-only call answered ALP_OK "
		       "(ping + soc_info[lifecycle=0x%02x] + RUN/STANDBY profiles + rnd) "
		       "-- no mutating surface called (alp_power_profile_set gated out)\n",
		       info.lifecycle);
	} else {
		printk("RESULT FAIL: at least one portable call did not return ALP_OK "
		       "(NOSUPPORT = backend not on this build; NOT_READY = controller "
		       "asleep/unreachable on this boot) -- see the per-call rc above\n");
	}

	return 0;
}
