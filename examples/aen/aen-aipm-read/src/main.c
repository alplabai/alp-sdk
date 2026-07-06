/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-aipm-read -- read the live RUN and STANDBY power/clock operating-point
 * profiles through the PORTABLE <alp/power.h> profile surface on the
 * E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * WHAT THE PROFILE SURFACE IS.  On SoCs like the E8 a system-controller
 * firmware owns the power and clock tree: a core never pokes PLL / DC-DC /
 * power-domain registers, it reads and writes a per-core operating-point
 * PROFILE instead (on the E8 that controller is the Secure Enclave and the
 * feature is aiPM -- Autonomous Intelligent Power Management).  The portable
 * surface exposes the two profiles every such platform keeps:
 *
 *   - ALP_POWER_PROFILE_RUN     -- the active operating point: CPU clock (Hz),
 *     core DC-DC rail (mV), which power domains / memory blocks stay powered,
 *     and the flexible-IO rail.
 *   - ALP_POWER_PROFILE_STANDBY -- the standby/off operating point plus the
 *     wake-event configuration.
 *
 * WHAT THIS APP DOES.  It calls alp_power_profile_get() for both profiles and
 * prints every portable field.  Frequencies arrive in Hz and rails in mV (the
 * backend maps the controller's ordinal encodings); the power_domains /
 * memory_blocks / wake_events bitmasks are IMPLEMENTATION-DEFINED (their bit
 * legends are SoC business, documented in the per-SoM HW reference), so this
 * app prints them raw -- portable code treats them as opaque tokens.
 *
 * SAFETY -- READ-ONLY.  alp_power_profile_get() never changes the operating
 * point.  Its mutating companion alp_power_profile_set() is DELIBERATELY NOT
 * CALLED: a wrong rail/clock value can brown out the rail or stall the core,
 * so profile writes need known-good target values from the per-SoM HW
 * reference + a recovery plan before any bench run (see the <alp/power.h>
 * warning).  alp_soc_secure_fw_ping() (<alp/hw_info.h>) runs first as the
 * liveness gate, exactly like the original heartbeat did.
 *
 * Both getters ride a bounded controller-mailbox round-trip, so the app never
 * hangs.  PASS gate: ping + both profile reads return ALP_OK.
 *
 * On builds without a profile-capable backend (native_sim, SoMs whose power
 * tree the application core owns directly) the same code links and runs; the
 * calls report ALP_ERR_NOSUPPORT and the app says so -- the portable-surface
 * contract this example exists to demonstrate.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/hw_info.h>
#include <alp/power.h>

static void print_profile(const char *name, const alp_power_profile_t *p, bool standby)
{
	printk("        cpu_clk_hz     = %u  (%u.%u MHz)\n",
	       p->cpu_clk_hz,
	       p->cpu_clk_hz / 1000000U,
	       (p->cpu_clk_hz % 1000000U) / 100000U);
	printk("        rail_mv        = %u mV\n", p->rail_mv);
	printk("        power_domains  = 0x%08x  (implementation-defined legend)\n", p->power_domains);
	printk("        memory_blocks  = 0x%08x  (implementation-defined legend)\n", p->memory_blocks);
	if (standby) {
		printk("        wake_events    = 0x%08x  (implementation-defined legend)\n",
		       p->wake_events);
	}
	printk("        io_mv          = %u mV\n", p->io_mv);
	(void)name;
}

int main(void)
{
	alp_status_t rc;
	bool         all_ok = true;

	printk("\n=== aen-aipm-read (portable operating-point profiles, read-only) ===\n");

	/*
	 * 0. Liveness: prove the platform's power/identity controller answers
	 *    before we trust the profile reads.
	 */
	rc = alp_soc_secure_fw_ping();
	printk("secure_fw_ping       : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);

	/*
	 * 1. RUN profile -- the live operating point.  Read-only; nothing is
	 *    written.
	 */
	alp_power_profile_t run = { 0 };

	rc = alp_power_profile_get(ALP_POWER_PROFILE_RUN, &run);
	printk("profile_get(RUN)     : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	if (rc == ALP_OK) {
		print_profile("RUN", &run, false);
	}

	/*
	 * 2. STANDBY profile -- the standby/off operating point + wake
	 *    configuration.  Read-only; nothing is written.
	 */
	alp_power_profile_t stby = { 0 };

	rc = alp_power_profile_get(ALP_POWER_PROFILE_STANDBY, &stby);
	printk("profile_get(STANDBY) : rc=%d\n", (int)rc);
	all_ok &= (rc == ALP_OK);
	if (rc == ALP_OK) {
		print_profile("STANDBY", &stby, true);
	}

	if (all_ok) {
		printk("RESULT PASS: portable profile surface returned RUN+STANDBY read-only "
		       "(RUN %u.%u MHz @ %u mV; STANDBY %u mV) -- alp_power_profile_set never "
		       "called\n",
		       run.cpu_clk_hz / 1000000U,
		       (run.cpu_clk_hz % 1000000U) / 100000U,
		       run.rail_mv,
		       stby.rail_mv);
	} else {
		printk("RESULT FAIL: ping or a profile read did not return ALP_OK "
		       "(NOSUPPORT = no profile-capable backend on this build; NOT_READY = "
		       "controller asleep/unreachable on this boot) -- see the per-call rc "
		       "above\n");
	}

	return 0;
}
