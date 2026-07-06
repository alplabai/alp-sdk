/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif SE aiPM operating-point-profile backend for the
 * alp_power_profile_get / alp_power_profile_set half of <alp/power.h>
 * on the Alif Ensemble E8 (E1M-AEN801).
 *
 * On the E8 the Secure Enclave (SE) owns the power and clock tree
 * through aiPM (Autonomous Intelligent Power Management): a core never
 * pokes PLL / DC-DC / power-domain registers, it reads and writes a
 * per-core RUN profile (run_profile_t) and OFF/standby profile
 * (off_profile_t) over the SE-service mailbox.  This backend maps
 * those vendor structs onto the portable alp_power_profile_t:
 *
 *   cpu_clk_hz    <-> cpu_clk_freq (RUN, clock_frequency_t ordinal) /
 *                     stby_clk_freq (STANDBY, scaled_clk_freq_t --
 *                     auto-selected by the SE, reported read-only)
 *   rail_mv       <-> dcdc_voltage (already millivolts)
 *   power_domains <-> power_domains  (raw vendor bit legend -- the
 *   memory_blocks <-> memory_blocks   portable surface documents both
 *                                     as implementation-defined)
 *   wake_events   <-> wakeup_events (STANDBY only)
 *   io_mv         <-> vdd_ioflex_3V3 (3300 / 1800)
 *
 * Registers at silicon_ref="alif:ensemble:e8" priority 100, above the
 * priority-0 "*" stub in zephyr_stub.c.  Class "power_profile" is
 * separate from class "power", so the pm_policy sleep backend keeps
 * winning request_sleep on AEN builds.
 *
 * GET is read-only (se_service_get_run_cfg / se_service_get_off_cfg;
 * the same two bench-proven aiPM getters the aen-aipm-read example
 * drives).  SET is a read-modify-write over se_service_set_run_cfg /
 * se_service_set_off_cfg per the portable contract: only non-zero
 * caller fields are applied, frequencies must map onto a
 * clock_frequency_t ordinal EXACTLY (never rounded), and the 76.8 /
 * 38.4 MHz RC-vs-XO ambiguity resolves from the profile's current
 * run_clk_src.  Every call bounds its wait inside hal_alif's
 * se_service.c, so nothing here hangs.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"

#if defined(CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE)

/* hal_alif SE service client (Apache-2.0).  Transitively provides
 * run_profile_t / off_profile_t and the aiPM enums (aipm.h). */
#include <se_service.h>

static alp_status_t se_rc_to_alp(int rc)
{
	if (rc == 0) {
		return ALP_OK;
	}
	switch (rc) {
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EAGAIN:
	case -EBUSY:
		return ALP_ERR_NOT_READY;
	default:
		return ALP_ERR_IO;
	}
}

/* clock_frequency_t ordinal -> Hz.  The ordinals are non-monotonic
 * (aipm.h); every arm is the vendor enum token, so a hal_alif bump
 * that renames one fails the build instead of mis-mapping. */
static uint32_t run_ordinal_to_hz(clock_frequency_t f)
{
	switch (f) {
	case CLOCK_FREQUENCY_800MHZ:
		return 800000000u;
	case CLOCK_FREQUENCY_400MHZ:
		return 400000000u;
	case CLOCK_FREQUENCY_300MHZ:
		return 300000000u;
	case CLOCK_FREQUENCY_200MHZ:
		return 200000000u;
	case CLOCK_FREQUENCY_160MHZ:
		return 160000000u;
	case CLOCK_FREQUENCY_120MHZ:
		return 120000000u;
	case CLOCK_FREQUENCY_100MHZ:
		return 100000000u;
	case CLOCK_FREQUENCY_80MHZ:
		return 80000000u;
	case CLOCK_FREQUENCY_60MHZ:
		return 60000000u;
	case CLOCK_FREQUENCY_50MHZ:
		return 50000000u;
	case CLOCK_FREQUENCY_20MHZ:
		return 20000000u;
	case CLOCK_FREQUENCY_10MHZ:
		return 10000000u;
	case CLOCK_FREQUENCY_76_8_RC_MHZ:
	case CLOCK_FREQUENCY_76_8_XO_MHZ:
		return 76800000u;
	case CLOCK_FREQUENCY_38_4_RC_MHZ:
	case CLOCK_FREQUENCY_38_4_XO_MHZ:
		return 38400000u;
	case CLOCK_FREQUENCY_DISABLED:
	default:
		return 0u;
	}
}

/* Hz -> clock_frequency_t, exact match only (the portable contract:
 * never round).  76.8 / 38.4 MHz exist as RC and XO ordinals; pick by
 * the profile's current HF clock source. */
static bool hz_to_run_ordinal(uint32_t hz, hfclock_t src, clock_frequency_t *out)
{
	bool xo = (src == CLK_SRC_HFXO);

	switch (hz) {
	case 800000000u:
		*out = CLOCK_FREQUENCY_800MHZ;
		return true;
	case 400000000u:
		*out = CLOCK_FREQUENCY_400MHZ;
		return true;
	case 300000000u:
		*out = CLOCK_FREQUENCY_300MHZ;
		return true;
	case 200000000u:
		*out = CLOCK_FREQUENCY_200MHZ;
		return true;
	case 160000000u:
		*out = CLOCK_FREQUENCY_160MHZ;
		return true;
	case 120000000u:
		*out = CLOCK_FREQUENCY_120MHZ;
		return true;
	case 100000000u:
		*out = CLOCK_FREQUENCY_100MHZ;
		return true;
	case 80000000u:
		*out = CLOCK_FREQUENCY_80MHZ;
		return true;
	case 60000000u:
		*out = CLOCK_FREQUENCY_60MHZ;
		return true;
	case 50000000u:
		*out = CLOCK_FREQUENCY_50MHZ;
		return true;
	case 20000000u:
		*out = CLOCK_FREQUENCY_20MHZ;
		return true;
	case 10000000u:
		*out = CLOCK_FREQUENCY_10MHZ;
		return true;
	case 76800000u:
		*out = xo ? CLOCK_FREQUENCY_76_8_XO_MHZ : CLOCK_FREQUENCY_76_8_RC_MHZ;
		return true;
	case 38400000u:
		*out = xo ? CLOCK_FREQUENCY_38_4_XO_MHZ : CLOCK_FREQUENCY_38_4_RC_MHZ;
		return true;
	default:
		return false;
	}
}

/* scaled_clk_freq_t (the SE-auto-selected standby clock) -> Hz.  Every
 * variant of the same frequency (RC-active / RC-standby / XO low-div /
 * XO high-div) reports the same Hz -- the portable field carries the
 * rate, not the source. */
static uint32_t scaled_ordinal_to_hz(scaled_clk_freq_t f)
{
	switch (f) {
	case SCALED_FREQ_RC_ACTIVE_76_8_MHZ:
	case SCALED_FREQ_RC_STDBY_76_8_MHZ:
		return 76800000u;
	case SCALED_FREQ_RC_ACTIVE_38_4_MHZ:
	case SCALED_FREQ_RC_STDBY_38_4_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_38_4_MHZ:
	case SCALED_FREQ_XO_HIGH_DIV_38_4_MHZ:
		return 38400000u;
	case SCALED_FREQ_RC_ACTIVE_19_2_MHZ:
	case SCALED_FREQ_RC_STDBY_19_2_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_19_2_MHZ:
	case SCALED_FREQ_XO_HIGH_DIV_19_2_MHZ:
		return 19200000u;
	case SCALED_FREQ_RC_ACTIVE_9_6_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_9_6_MHZ:
	case SCALED_FREQ_XO_HIGH_DIV_9_6_MHZ:
		return 9600000u;
	case SCALED_FREQ_RC_ACTIVE_4_8_MHZ:
	case SCALED_FREQ_RC_STDBY_4_8_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_4_8_MHZ:
		return 4800000u;
	case SCALED_FREQ_RC_ACTIVE_2_4_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_2_4_MHZ:
	case SCALED_FREQ_XO_HIGH_DIV_2_4_MHZ:
		return 2400000u;
	case SCALED_FREQ_RC_ACTIVE_1_2_MHZ:
	case SCALED_FREQ_RC_STDBY_1_2_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_1_2_MHZ:
		return 1200000u;
	case SCALED_FREQ_RC_ACTIVE_0_6_MHZ:
	case SCALED_FREQ_RC_STDBY_0_6_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_0_6_MHZ:
	case SCALED_FREQ_XO_HIGH_DIV_0_6_MHZ:
		return 600000u;
	case SCALED_FREQ_RC_STDBY_0_3_MHZ:
	case SCALED_FREQ_XO_LOW_DIV_0_3_MHZ:
		return 300000u;
	case SCALED_FREQ_RC_STDBY_0_075_MHZ:
		return 75000u;
	default:
		return 0u;
	}
}

static uint32_t ioflex_to_mv(ioflex_mode_t io)
{
	return (io == IOFLEX_LEVEL_1V8) ? 1800u : 3300u;
}

static alp_status_t alif_profile_get(alp_power_profile_id_t which, alp_power_profile_t *out)
{
	if (which == ALP_POWER_PROFILE_RUN) {
		run_profile_t run = { 0 };
		int           rc  = se_service_get_run_cfg(&run);

		if (rc != 0) {
			return se_rc_to_alp(rc);
		}
		out->cpu_clk_hz    = run_ordinal_to_hz(run.cpu_clk_freq);
		out->rail_mv       = run.dcdc_voltage;
		out->power_domains = run.power_domains;
		out->memory_blocks = run.memory_blocks;
		out->wake_events   = 0u;
		out->io_mv         = ioflex_to_mv(run.vdd_ioflex_3V3);
		return ALP_OK;
	}

	off_profile_t off = { 0 };
	int           rc  = se_service_get_off_cfg(&off);

	if (rc != 0) {
		return se_rc_to_alp(rc);
	}
	out->cpu_clk_hz    = scaled_ordinal_to_hz(off.stby_clk_freq);
	out->rail_mv       = off.dcdc_voltage;
	out->power_domains = off.power_domains;
	out->memory_blocks = off.memory_blocks;
	out->wake_events   = off.wakeup_events;
	out->io_mv         = ioflex_to_mv(off.vdd_ioflex_3V3);
	return ALP_OK;
}

/* Map an io_mv request onto the ioflex rail.  Only the two levels the
 * silicon has are accepted -- the portable contract never rounds. */
static bool io_mv_to_ioflex(uint32_t io_mv, ioflex_mode_t *out)
{
	if (io_mv == 3300u) {
		*out = IOFLEX_LEVEL_3V3;
		return true;
	}
	if (io_mv == 1800u) {
		*out = IOFLEX_LEVEL_1V8;
		return true;
	}
	return false;
}

static alp_status_t alif_profile_set(alp_power_profile_id_t which, const alp_power_profile_t *p)
{
	if (which == ALP_POWER_PROFILE_RUN) {
		/* Read-modify-write: fetch the live profile, patch exactly the
		 * caller's non-zero fields, write it back. */
		run_profile_t run = { 0 };
		int           rc  = se_service_get_run_cfg(&run);

		if (rc != 0) {
			return se_rc_to_alp(rc);
		}
		if (p->cpu_clk_hz != 0u &&
		    !hz_to_run_ordinal(p->cpu_clk_hz, run.run_clk_src, &run.cpu_clk_freq)) {
			return ALP_ERR_INVAL;
		}
		if (p->rail_mv != 0u) {
			run.dcdc_voltage = p->rail_mv;
		}
		if (p->power_domains != 0u) {
			run.power_domains = p->power_domains;
		}
		if (p->memory_blocks != 0u) {
			run.memory_blocks = p->memory_blocks;
		}
		if (p->io_mv != 0u && !io_mv_to_ioflex(p->io_mv, &run.vdd_ioflex_3V3)) {
			return ALP_ERR_INVAL;
		}
		return se_rc_to_alp(se_service_set_run_cfg(&run));
	}

	/* STANDBY.  The standby clock (stby_clk_freq) is auto-selected by
	 * the SE (aipm.h) -- a caller-supplied cpu_clk_hz cannot be
	 * realised, so it is rejected rather than silently dropped. */
	if (p->cpu_clk_hz != 0u) {
		return ALP_ERR_INVAL;
	}

	off_profile_t off = { 0 };
	int           rc  = se_service_get_off_cfg(&off);

	if (rc != 0) {
		return se_rc_to_alp(rc);
	}
	if (p->rail_mv != 0u) {
		off.dcdc_voltage = p->rail_mv;
	}
	if (p->power_domains != 0u) {
		off.power_domains = p->power_domains;
	}
	if (p->memory_blocks != 0u) {
		off.memory_blocks = p->memory_blocks;
	}
	if (p->wake_events != 0u) {
		off.wakeup_events = p->wake_events;
	}
	if (p->io_mv != 0u && !io_mv_to_ioflex(p->io_mv, &off.vdd_ioflex_3V3)) {
		return ALP_ERR_INVAL;
	}
	return se_rc_to_alp(se_service_set_off_cfg(&off));
}

static const alp_power_profile_ops_t _ops = {
	.get = alif_profile_get,
	.set = alif_profile_set,
};

ALP_BACKEND_REGISTER(power_profile,
                     alif_se,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE */
