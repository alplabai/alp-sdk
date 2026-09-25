/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-cm33-deepx-rail -- sequence the DEEPX DX-M1 core rail (DA9292 CH2,
 * 0.75 V) from the CM33 system-manager, in CM33-boot mode.
 *
 * WHY THIS EXAMPLE EXISTS.  On the E1M-V2N / V2N-M1 SoM the boot-CPU is a
 * hardware strap: RZ/V2N pin BOOTSELCPU (RZ/V2N HW manual
 * R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table 1.9-1) selects LOW = CM33 cold
 * boot, HIGH = CA55 cold boot, and is driven by ACT88760 GPIO5 (net
 * V2N_BOOT_CPU_SEL -- the power sequencer's own CMI drives it HIGH by
 * default, ~8.6 ms after MODULE_EN, so CA55-cold-boot is the power-on
 * default).  In the DEFAULT config, U-Boot's board_late_init() (the 0004
 * patch) sequences this same rail on the A55 before releasing M1_RESET,
 * and CA55/Linux is thereafter the sole master of RIIC8/BRD_I2C -- see
 * metadata/e1m_modules/v2n/core-ownership.yaml.
 *
 * If BOOTSELCPU is instead strapped low, the CM33 cold-boots FIRST (from
 * xSPI or SCIF download -- the only two boot sources CM33-cold-boot
 * supports) and must release the CA55 itself, later.  THIS app is what
 * runs during that window: it masters RIIC8 and programs the DA9292
 * exactly like U-Boot 0004 does, using the SAME portable chip-driver
 * function (da9292_ch2_sequence(), <alp/chips/da9292.h>) so the two paths
 * can never drift apart.  Ownership is TIME-SLICED, never concurrent --
 * this app's window ends (see the TODO at the bottom) BEFORE the CA55
 * starts, at which point U-Boot 0004 runs again as a warm, idempotent
 * VERIFY (its program phase is a no-op when CH2 is already at target).
 * metadata/e1m_modules/v2n/power-tree.yaml's `boot_modes.cm33_boot` and
 * core-ownership.yaml's `boot_mode_core` qualifier record this so
 * scripts/gen_power_tree.py's cross_check() can still reject a real
 * dual-master (non-time-sliced) config.
 *
 * DO NOT wire this app's board overlay into any OTHER CM33 example: it
 * re-enables &i2c8, which every other CM33 app must leave disabled
 * (RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive outside this one window).
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/chips/da9292.h"
#include "alp/chips/v2n_power_tree.h"

/* alp,pin-array indices this app's board overlay defines (see
 * boards/alp_e1m_v2m101_m33_sm_r9a09g056n48gbg_cm33.overlay) -- NOT the
 * usual E1M positional pinout, since P64/P65 are SoM-internal pads. */
#define PIN_ID_DEEPX_CORE_0P75_EN 0u /* P64, output */
#define PIN_ID_DEEPX_PWR_EN_REQ   1u /* P65, input  */

/* Zephyr delay callback for da9292_ch2_sequence() -- keeps the driver
 * OS-agnostic (Linux/U-Boot pass their own wrappers around the same
 * function pointer type). Wraps the portable alp_delay_us() (Zephyr
 * backend: k_busy_wait()/k_usleep() depending on duration) rather than
 * calling a Zephyr kernel API directly, so this app stays inside the
 * portable alp headers end to end. */
static void zephyr_delay_us(void *user, uint32_t us)
{
	(void)user;
	alp_delay_us(us);
}

/* Human-readable name for a da9292_ch2_seq_step_t -- every abort path the
 * driver can take, so a console-attached run always says exactly where it
 * stopped instead of just a bare error code. */
static const char *step_name(da9292_ch2_seq_step_t step)
{
	switch (step) {
	case DA9292_SEQ_OK:
		return "OK";
	case DA9292_SEQ_ERR_ARGS:
		return "bad cfg / no limits table / target outside window";
	case DA9292_SEQ_ERR_IDENTITY:
		return "PMC_DEV_ID mismatch or unreadable";
	case DA9292_SEQ_ERR_STATUS01:
		return "PMC_STATUS_01 != 0x00 (thermal/UVLO latched)";
	case DA9292_SEQ_ERR_PREREAD:
		return "CTRL_01 / VOUT pre-program read failed";
	case DA9292_SEQ_ERR_CLEAR_VSTEP:
		return "VSTEP+EN clear write/read-back failed";
	case DA9292_SEQ_ERR_PROGRAM_VOUT:
		return "VOUT_CH2 program write/read-back failed";
	case DA9292_SEQ_ERR_CH1_DISTURBED:
		return "CH1 VOUT changed or CH1_EN cleared";
	case DA9292_SEQ_ERR_EVENTS:
		return "EVENT_00/01 pre-clear read failed";
	case DA9292_SEQ_ERR_NO_REQUEST:
		return "DEEPX_PWR_EN_REQ (P65) never went high";
	case DA9292_SEQ_ERR_VSTEP_AT_ENABLE:
		return "CH2_VSTEP set right before enable";
	case DA9292_SEQ_ERR_ENABLE:
		return "CH2_EN write failed";
	case DA9292_SEQ_ERR_PG_TIMEOUT:
		return "no CH2_PG (or a fault) within the timeout";
	case DA9292_SEQ_ERR_PG_DROPPED:
		return "PG lost after DEEPX_CORE_0P75_EN went high";
	default:
		return "unknown step";
	}
}

int main(void)
{
	printf("[cm33-deepx-rail] V2N-M1 CM33-boot DEEPX 0.75V rail sequencer\n");
	printf("[cm33-deepx-rail] this app is ONLY correct if BOOTSELCPU strapped this boot "
	       "as cm33_boot -- see the file header if you are not sure\n");

	/* BRD_I2C / RIIC8: this app's board overlay is the ONLY thing that
	 * enables &i2c8 for a CM33 build on this SoM -- see its header.
	 * ALP_I2C_CONFIG_DEFAULT's 100 kHz standard-mode bitrate is kept
	 * as-is (not raised to Fast): alp_i2c_open() calls i2c_configure()
	 * at runtime, which would otherwise override the overlay's
	 * I2C_BITRATE_STANDARD and undo the slower RIIC8 clock this bus
	 * bench-needs (see the overlay header, mirrors U-Boot 0006's
	 * CKS(5)). */
	alp_i2c_config_t i2c_cfg = ALP_I2C_CONFIG_DEFAULT(0);
	alp_i2c_t       *i2c     = alp_i2c_open(&i2c_cfg);
	if (i2c == NULL) {
		printf("[cm33-deepx-rail] alp_i2c_open failed: err=%d -- check the board "
		       "overlay enables &i2c8\n",
		       (int)alp_last_error());
		return 0;
	}

	/* P64 DEEPX_CORE_0P75_EN: output, start low (rail stays down until
	 * the sequence below confirms CH2 power-good).  Write false BEFORE
	 * configure, then again after -- the same write-before-configure-
	 * then-write-after pattern chips/tas2563/tas2563.c's SD_N handling
	 * uses (see the long comment above its `if (sd_n != NULL)` block)
	 * for the identical reason: `alp_gpio_configure(..., ALP_GPIO_OUTPUT,
	 * ...)` deliberately does not force a level (see `_to_gpio_flags()`,
	 * src/backends/gpio/zephyr_drv.c), so on a backend whose direction
	 * switch alone can glitch the data register, only a write already in
	 * flight before that switch closes the window; the second write
	 * covers a backend (gpio_emul among them) that drops a write issued
	 * before the pin is configured as output. */
	alp_gpio_t  *core_en = alp_gpio_open(PIN_ID_DEEPX_CORE_0P75_EN);
	alp_status_t s       = ALP_ERR_NOT_READY;
	if (core_en != NULL) {
		s = alp_gpio_write(core_en, false);
		if (s == ALP_OK) s = alp_gpio_configure(core_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
		if (s == ALP_OK) s = alp_gpio_write(core_en, false);
	}
	if (s != ALP_OK) {
		printf("[cm33-deepx-rail] P64 (DEEPX_CORE_0P75_EN) open/configure failed: %d\n", (int)s);
		goto out_core_en; /* alp_gpio_close(NULL) is a safe no-op if open() itself failed */
	}

	/* P65 DEEPX_PWR_EN_REQ: input, no pull -- it's an ACT88760 GPIO7
	 * push-pull output (Buck5 power-good), driven regardless of which
	 * CPU is reading it. */
	alp_gpio_t *pwr_en_req = alp_gpio_open(PIN_ID_DEEPX_PWR_EN_REQ);
	s = (pwr_en_req != NULL) ? alp_gpio_configure(pwr_en_req, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE)
	                         : ALP_ERR_NOT_READY;
	if (s != ALP_OK) {
		printf("[cm33-deepx-rail] P65 (DEEPX_PWR_EN_REQ) open/configure failed: %d\n", (int)s);
		goto out_pwr_en_req; /* alp_gpio_close(NULL) is a safe no-op if open() itself failed */
	}

	/* da9292_init() only probes PMC_DEV_ID -- no register write yet. */
	da9292_t ctx;
	s = da9292_init(&ctx, i2c, DA9292_I2C_ADDR_V2N);
	if (s != ALP_OK) {
		printf("[cm33-deepx-rail] da9292_init failed: %d (DA9292 not answering on "
		       "BRD_I2C?)\n",
		       (int)s);
		goto out_pwr_en_req;
	}

	/* Unlocks every guarded write; da9292_ch2_sequence() refuses
	 * (ALP_ERR_NOSUPPORT) with none installed.  V2M101 is v2n-m1 family.
	 * The generated macro is a brace-init-list (array initializer), not
	 * a pointer -- static storage per da9292_set_limits()'s contract:
	 * ch_limits must outlive ctx. */
	static const pmic_rail_limit_t da9292_ch_limits[DA9292_CH_COUNT] =
	    V2N_M1_POWER_DA9292_CH_LIMITS_INIT;
	s = da9292_set_limits(&ctx, da9292_ch_limits);
	if (s != ALP_OK) {
		printf("[cm33-deepx-rail] da9292_set_limits failed: %d\n", (int)s);
		goto out_pwr_en_req;
	}

	/* Mirrors U-Boot 0004's alp_deepx_rail_bringup() step for step -- see
	 * da9292_ch2_sequence()'s doc comment for the full 13-step walk. */
	struct da9292_ch2_seq_cfg cfg = {
		.target_mv = 750u,
		/* One shared identity macro across both families -- see
		 * v2n_power_tree.h: DEV_ID/REV_ID/CFG_REV are emitted once,
		 * prefixed from families[0] ("v2n") regardless of which
		 * family's *_CH_LIMITS_INIT table is in use below. */
		.expected_dev_id = V2N_POWER_DA9292_DEV_ID,
		.pwr_en_req      = pwr_en_req,
		.core_en         = core_en,
		.m1_reset        = NULL, /* PCIe bring-up releases M1_RESET, not this app */
		.req_timeout_ms  = 500u,
		.pg_timeout_ms   = 20u,
		.pg_settle_ms    = 5u,
		.delay           = zephyr_delay_us,
		.delay_user      = NULL,
	};
	struct da9292_ch2_seq_result res;
	s = da9292_ch2_sequence(&ctx, &cfg, &res);

	printf("[cm33-deepx-rail] da9292_ch2_sequence -> %d, step=%s%s\n",
	       (int)s,
	       step_name(res.step),
	       res.already_programmed ? " (warm: already at target, program phase skipped)" : "");
	if (s == ALP_OK) {
		printf("[cm33-deepx-rail] VDD_0P75 up, rail_up=%d, CH2_PG confirmed, "
		       "DEEPX_CORE_0P75_EN (P64) high\n",
		       (int)res.rail_up);
	} else {
		printf("[cm33-deepx-rail] rail sequence FAILED -- status_00=0x%02x status_01=0x%02x "
		       "event_00=0x%02x ctrl_01=0x%02x\n",
		       res.status_00,
		       res.status_01,
		       res.event_00,
		       res.ctrl_01);
	}

	/*
	 * TODO(alp-sdk#2289): release the CA55 here on ALP_OK.  This app
	 * masters RIIC8 only until that handoff (metadata/e1m_modules/v2n/
	 * power-tree.yaml boot_modes.cm33_boot's `handover`); once the CA55
	 * starts, U-Boot 0004 runs its own (idempotent, verify-only on a
	 * warm rail) pass and Linux takes RIIC8 over exclusively. Releasing
	 * the CA55 needs the actual RZ/V2N CPU-reset-control register(s) --
	 * NOT confirmed against the hardware manual or bench-verified yet,
	 * so this app deliberately stops here rather than guess at a SoC
	 * register write.  Do not add one without a hardware-manual
	 * citation; see the issue for the follow-up.
	 */
	printf("[cm33-deepx-rail] CA55-release handoff is NOT implemented -- see "
	       "alp-sdk#2289; stopping here\n");

out_pwr_en_req:
	alp_gpio_close(pwr_en_req);
out_core_en:
	alp_gpio_close(core_en);
	alp_i2c_close(i2c); /* no separate out_i2c: label -- nothing jumps here directly */
	printf("[cm33-deepx-rail] done\n");
	return 0;
}
