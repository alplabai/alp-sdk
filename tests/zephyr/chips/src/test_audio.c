/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio chip smokes: pdm_mic (block helper, v0.1 surface-only stub),
 * tas2563 (TI Class-D amp), and the v0.5 §D.audio batch mics/codecs.
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/ztest.h>

#include "alp/blocks/pdm_mic.h"
#include "alp/chips/es8388.h"
#include "alp/chips/ics_43434.h"
#include "alp/chips/inmp441.h"
#include "alp/chips/max98357a.h"
#include "alp/chips/tas2563.h"
#include "alp/chips/tlv320aic3204.h"
#include "alp/chips/wm8960.h"
#include "alp/e1m_pinout.h"
#include "alp/i2s.h"
#include "alp/peripheral.h"
#include "fakes.h"

/* ------------------------------------------------------------------ */
/* pdm_mic (v0.2 helper — surface only; impl returns NOSUPPORT in v0.1) */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pdm_mic_open_returns_null_in_v01)
{
	alp_pdm_mic_t *mic = alp_pdm_mic_open(&(alp_pdm_mic_config_t){
	    .peripheral_id  = 0,
	    .sample_rate_hz = 16000,
	    .channels       = ALP_PDM_MIC_MONO,
	    .sample_bits    = 16,
	});
	zassert_is_null(mic, "v0.1 stub must return NULL until v0.2 audio lands");
}

ZTEST(alp_chips, test_pdm_mic_calls_return_nosupport)
{
	/* Even with a NULL handle (v0.1 contract), the read/set_gain
     * surface must reply ALP_ERR_NOSUPPORT — the stub asserts the
     * shape, not real arithmetic. */
	int16_t buf[16] = { 0 };
	size_t  n       = 999;
	zassert_equal(alp_pdm_mic_read(NULL, buf, sizeof buf / sizeof buf[0], &n, 0),
	              ALP_ERR_NOSUPPORT);
	zassert_equal(n, 0u, "out_frames must be zeroed by the stub");
	zassert_equal(alp_pdm_mic_set_gain(NULL, 0, 0), ALP_ERR_NOSUPPORT);
	alp_pdm_mic_close(NULL); /* must not crash. */
}

/* ------------------------------------------------------------------ */
/* tas2563 -- TI smart Class-D speaker amplifier                      */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tas2563_init_null_args)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	/* sd_n is optional in the driver -- not required to be non-NULL. */
	zassert_equal(tas2563_init(NULL, bus, 0x4Du, NULL), ALP_ERR_INVAL);
	zassert_equal(tas2563_init(&ctx, NULL, 0x4Du, NULL), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tas2563_calls_reject_uninitialised)
{
	tas2563_t ctx = { 0 };
	uint8_t   rev;

	zassert_equal(tas2563_read_revision(&ctx, &rev), ALP_ERR_NOT_READY);
	zassert_equal(tas2563_set_mode(&ctx, (tas2563_mode_t)0), ALP_ERR_NOT_READY);
	zassert_equal(tas2563_set_hw_enable(&ctx, true), ALP_ERR_NOT_READY);
}

/* #739: every documented strap address (Table 7-3) must be accepted;
 * anything else -- including the two out-of-range/UB-adjacent boundary
 * probes 0x80 and 0xFF -- must be rejected before any bus access.
 *
 * #1846: TAS2563_I2C_ADDR_BROADCAST (0x48) moved from valid[] to
 * invalid[] -- it is a real datasheet address but not a single,
 * individually-addressable chip, and init's own probe both reads AND
 * writes through ctx->addr (select_page() writes the page register
 * before the REVID read).  Accepting it let init -- and every later
 * read_revision()/set_mode() call, all gated only on ctx->initialised,
 * never re-checking the address -- silently land on whatever else is
 * strapped to 0x48 on the bus (a real EVK pre-respin had an INA236
 * there); see test_tas2563_init_rejects_broadcast_address below for
 * the dedicated regression. */
ZTEST(alp_chips, test_tas2563_init_validates_address_strap_range)
{
	tas2563_t ctx;

	zassert_equal(tas2563_init(&ctx, NULL, TAS2563_I2C_ADDR_GND_DIRECT, NULL),
	              ALP_ERR_INVAL,
	              "NULL bus is checked first regardless of address");

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	/* Every strap-documented address is accepted by the range check
	 * (the driver may still fail NOT_READY past that point on
	 * native_sim with no real amp on the bus -- only the validation
	 * gate is under test here). */
	const uint8_t valid[] = {
		TAS2563_I2C_ADDR_GND_DIRECT,
		TAS2563_I2C_ADDR_GND_PULL,
		TAS2563_I2C_ADDR_VDD_PULL,
		TAS2563_I2C_ADDR_VDD_DIRECT,
	};
	for (size_t i = 0; i < ARRAY_SIZE(valid); ++i) {
		alp_status_t s = tas2563_init(&ctx, bus, valid[i], NULL);
		zassert_not_equal(
		    s, ALP_ERR_INVAL, "addr 0x%02x must pass the strap-range check", valid[i]);
	}

	/* 0x00, the low neighbor 0x4B, the high neighbor 0x50, the
	 * 0x7F/0x80/0xFF domain boundaries, and the broadcast address
	 * itself (#1846) are all outside the individually-addressable
	 * strap set. */
	const uint8_t invalid[] = {
		0x00u, 0x4Bu, TAS2563_I2C_ADDR_BROADCAST, 0x50u, 0x7Fu, 0x80u, 0xFFu
	};
	for (size_t i = 0; i < ARRAY_SIZE(invalid); ++i) {
		zassert_equal(tas2563_init(&ctx, bus, invalid[i], NULL),
		              ALP_ERR_INVAL,
		              "addr 0x%02x must be rejected",
		              invalid[i]);
	}

	alp_i2c_close(bus);
}

/* #1846 regression: a bare, explicit assertion that init at the
 * global broadcast address is refused -- kept separate from the
 * strap-range sweep above so this exact defect (broadcast init
 * clobbering an unrelated 0x48 device, e.g. an INA236) has one
 * test that names it and cannot be silently widened away. */
ZTEST(alp_chips, test_tas2563_init_rejects_broadcast_address)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(tas2563_init(&ctx, bus, TAS2563_I2C_ADDR_BROADCAST, NULL),
	              ALP_ERR_INVAL,
	              "init must refuse the write-only broadcast address (#1846): 0x48 does "
	              "not pin down one physical chip, and every op this driver exposes but "
	              "set_hw_enable() both reads and writes through ctx->addr, which a "
	              "device sharing 0x48 (e.g. an INA236) would answer instead of the amp");

	alp_i2c_close(bus);
}

/* ------------------------------------------------------------------ */
/* tas2563 -- v0.3.x register-protocol coverage against the fake       */
/* ------------------------------------------------------------------ */
/* Gated on the fake_tas2563 overlay node so this block compiles to
 * nothing if that emul target is ever taken out of the overlay.
 *
 * Expected register values below are computed from the datasheet POR
 * value the fake seeds (SLASET3D "[reset=..]" headings, p.65-94)
 * masked with the field the driver owns -- so a failing assertion
 * means the driver wrote the wrong field, not merely a different
 * byte.  Nothing here has been observed on silicon: no speaker is
 * connected to the bench (maintainer, 2026-09-08). */

#if DT_NODE_EXISTS(DT_NODELABEL(fake_tas2563))

#define TAS_FAKE_ADDR TAS2563_I2C_ADDR_GND_PULL /* 0x4D, matches the overlay. */

/* Register addresses re-declared here (they are private to
 * chips/tas2563/tas2563.c) so an assertion reads as the datasheet
 * register it checks.  SLASET3D section 7.5.1, p.64-65. */
#define TAS_REG_PAGE      0x00u
#define TAS_REG_SW_RESET  0x01u
#define TAS_REG_PWR_CTL   0x02u
#define TAS_REG_PB_CFG1   0x03u
#define TAS_REG_MISC_CFG1 0x04u
#define TAS_REG_TDM_CFG0  0x06u
#define TAS_REG_TDM_CFG1  0x07u
#define TAS_REG_TDM_CFG2  0x08u
#define TAS_REG_TDM_CFG5  0x0Bu
#define TAS_REG_TDM_CFG6  0x0Cu
#define TAS_REG_INT_LTCH0 0x24u
#define TAS_REG_INT_LTCH1 0x25u
#define TAS_REG_INT_LTCH3 0x26u
#define TAS_REG_INT_LTCH4 0x27u
#define TAS_REG_INT_CLK   0x30u

/* alp_gpio_open() pin ids from the overlay's alp,pin-array. */
#define TAS_PIN_IRQ_N 2u
#define TAS_PIN_SD_N  3u

static const struct device *tas_gpio_dev(void)
{
	return DEVICE_DT_GET(DT_NODELABEL(gpio_emul0));
}

/* Reset the fake, pre-seed PWR_CTL, init the driver, then clear the
 * write log so each test sees only its own bus traffic. */
static alp_i2c_t *tas_init(tas2563_t *ctx, uint8_t pwr_ctl_seed, alp_gpio_t *sd_n)
{
	fake_tas2563_reset();
	fake_tas2563_set_reg(TAS_REG_PWR_CTL, pwr_ctl_seed);
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(tas2563_init(ctx, bus, TAS_FAKE_ADDR, sd_n),
	              ALP_OK,
	              "init must succeed against the fake at 0x%02x",
	              TAS_FAKE_ADDR);
	fake_tas2563_log_reset();
	return bus;
}

/* PWR_CTL's reset value is Eh -- MODE is already 10b (software
 * shutdown), SLASET3D 7.5.4 Table 7-104 p.66.  Seeding MODE = 00b
 * (ACTIVE) makes "init forced it back" observable; that is the
 * warm-restart case, where SD_N was never dropped and register state
 * survived from the previous firmware. */
ZTEST(alp_chips, test_tas2563_init_parks_amp_in_software_shutdown)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x00u, NULL);

	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL),
	              0x02u,
	              "init must leave PWR_CTL.MODE = 10b (software shutdown) and touch "
	              "nothing else in the byte");

	alp_i2c_close(bus);
}

/* PWR_CTL.MODE is bits 1..0 ONLY; bits 3..2 are ISNS_PD/VSNS_PD
 * (Table 7-104, p.66).  Seeding both sense bits set makes a too-wide
 * mask visible: a 0x07 mask would clear VSNS_PD as a side effect of
 * going ACTIVE and silently power the voltage-sense block up. */
ZTEST(alp_chips, test_tas2563_set_mode_preserves_iv_sense_power_bits)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Cu, NULL);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x0Eu);

	zassert_equal(tas2563_set_mode(&ctx, TAS2563_MODE_ACTIVE), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL),
	              0x0Cu,
	              "ACTIVE must write MODE=00b and leave ISNS_PD/VSNS_PD set");

	zassert_equal(tas2563_set_mode(&ctx, TAS2563_MODE_MUTE), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x0Du);

	zassert_equal(tas2563_set_mode(&ctx, TAS2563_MODE_SHUTDOWN), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x0Eu);

	alp_i2c_close(bus);
}

/* MODE = 11b is "Load Diagnostics followed by device ACTIVE"
 * (7.3.11.5, p.35): it drives the speaker terminals and then leaves
 * the amp switching.  A caller must not reach it by casting a stray
 * integer into tas2563_mode_t. */
ZTEST(alp_chips, test_tas2563_set_mode_refuses_load_diagnostics_encoding)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x00u, NULL);

	const uint32_t before = fake_tas2563_write_count(TAS_REG_PWR_CTL);
	zassert_equal(tas2563_set_mode(&ctx, (tas2563_mode_t)0x03u),
	              ALP_ERR_INVAL,
	              "MODE=11b (load diagnostics then ACTIVE) must be refused");
	zassert_equal(tas2563_set_mode(&ctx, (tas2563_mode_t)0xFFu), ALP_ERR_INVAL);
	zassert_equal(fake_tas2563_write_count(TAS_REG_PWR_CTL),
	              before,
	              "a refused mode must not reach the bus at all");

	alp_i2c_close(bus);
}

/* BOOK survives software shutdown along with the rest of the register
 * state (7.3.11.2, p.34), so a device left mid-tuning by a previous
 * firmware comes up with a non-zero BOOK.  init must select book 0 as
 * well as page 0, or REVID and PWR_CTL address coefficient space.
 * The fake gives book 0 / page 0 a backing store and nowhere else, so
 * "the park write actually landed" is the observable that proves it. */
ZTEST(alp_chips, test_tas2563_init_selects_book0_not_just_page0)
{
	fake_tas2563_reset();
	fake_tas2563_set_reg(TAS_REG_PWR_CTL, 0x00u); /* MODE = 00b, ACTIVE */
	fake_tas2563_force_paging(8u, 5u);

	tas2563_t  ctx;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(tas2563_init(&ctx, bus, TAS_FAKE_ADDR, NULL), ALP_OK);

	zassert_equal(fake_tas2563_cur_book(), 0u, "init must select book 0");
	zassert_equal(fake_tas2563_cur_page(), 0u, "init must select page 0");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL),
	              0x02u,
	              "the park write must have landed on book 0 / page 0, not in "
	              "whatever page the previous firmware left selected");

	alp_i2c_close(bus);
}

/* #2077 (SW-reset addition): tas2563_init() must issue the software
 * reset (SW_RESET, SLASET3D §7.5.3) after selecting book 0 / page 0
 * and before any other configuration write -- SLAA954 "TAS2563 End
 * System Integration Guide" §3.1 Case 1.  Pin the whole write sequence
 * a clean init issues via the fake's ordered log: PAGE=0, BOOK=0
 * (paging), SW_RESET=1 (this fix), PWR_CTL park -- SW_RESET strictly
 * between the paging writes and the park write. */
ZTEST(alp_chips, test_tas2563_init_issues_sw_reset_before_other_configuration)
{
	fake_tas2563_reset();

	tas2563_t  ctx;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	fake_tas2563_log_reset();

	zassert_equal(tas2563_init(&ctx, bus, TAS_FAKE_ADDR, NULL), ALP_OK);

	static const struct fake_tas2563_write expected[] = {
		{ 0u, 0u, TAS_REG_PAGE, 0x00u },     /* select_book0_page0(): PAGE = 0 */
		{ 0u, 0u, 0x7Fu, 0x00u },            /* select_book0_page0(): BOOK = 0 */
		{ 0u, 0u, TAS_REG_SW_RESET, 0x01u }, /* the software reset this fix adds */
		{ 0u, 0u, TAS_REG_PWR_CTL, 0x0Eu },  /* park write, RMW over the POR-default
						        0x0Eu seed_defaults() leaves in PWR_CTL --
						        ISNS_PD/VSNS_PD (already 1) survive the
						        mask, only MODE[1:0] changes (already 10b) */
	};
	zassert_equal(fake_tas2563_log_len(),
	              ARRAY_SIZE(expected),
	              "unexpected write count during init -- SW_RESET missing or extra "
	              "writes appeared");
	for (size_t i = 0; i < ARRAY_SIZE(expected); ++i) {
		const struct fake_tas2563_write *w = fake_tas2563_log(i);
		zassert_not_null(w, "log[%zu] missing", i);
		zassert_equal(w->book, expected[i].book, "log[%zu].book", i);
		zassert_equal(w->page, expected[i].page, "log[%zu].page", i);
		zassert_equal(w->reg, expected[i].reg, "log[%zu].reg", i);
		zassert_equal(w->val, expected[i].val, "log[%zu].val", i);
	}

	alp_i2c_close(bus);
}

/* #2077 (SW-reset addition): the settle wait after SW_RESET runs
 * unconditionally, even with sd_n == NULL, where there is no
 * hardware-reset wait at all -- this is the ONLY wait on that path, so
 * its floor is exactly 1x TAS2563_RESET_SETTLE_US, unlike the 2x floor
 * of the sd_n-owned test above. */
ZTEST(alp_chips, test_tas2563_init_settles_after_sw_reset_when_sd_n_not_owned)
{
	fake_tas2563_reset();

	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	tas2563_t ctx;
	uint32_t  t0 = k_cycle_get_32();
	zassert_equal(tas2563_init(&ctx, bus, TAS_FAKE_ADDR, NULL), ALP_OK);
	uint64_t elapsed_us = k_cyc_to_us_floor64(k_cycle_get_32() - t0);

	zassert_true(elapsed_us >= TAS2563_RESET_SETTLE_US,
	             "tas2563_init() with sd_n == NULL took %llu us, want >= %u us "
	             "(TAS2563_RESET_SETTLE_US after the mandatory software reset) -- "
	             "looks like the settle wait was dropped",
	             (unsigned long long)elapsed_us,
	             TAS2563_RESET_SETTLE_US);

	alp_i2c_close(bus);
}

/* #2077: the connectivity probe must propagate the bus's own status
 * instead of remapping every failure to a hardcoded ALP_ERR_NOT_READY.
 * Arm the fake to NACK the very first bus write init issues -- the
 * PAGE=0 write inside select_book0_page0() -- and confirm the NACK
 * surfaces as ALP_ERR_IO, the code alp_i2c_write() documents for
 * "NACK / bus fault" (include/alp/peripheral.h). */
ZTEST(alp_chips, test_tas2563_init_propagates_bus_status_on_probe_failure)
{
	fake_tas2563_reset();
	fake_tas2563_fail_write_at(0u, 0u, TAS_REG_PAGE);

	tas2563_t  ctx;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	zassert_equal(tas2563_init(&ctx, bus, TAS_FAKE_ADDR, NULL),
	              ALP_ERR_IO,
	              "a NACK on the probe's first write must surface as ALP_ERR_IO, the "
	              "status the bus call actually returned -- not a hardcoded "
	              "ALP_ERR_NOT_READY");

	alp_i2c_close(bus);
}

/* AMP_LEVEL is PB_CFG1 bits 5..1, reset 10h = 16.0 dBV / 8.92 Vpk;
 * Table 7-105 (p.67) lists 01h..1Ch and marks 1Dh-1Fh Reserved, with
 * 00h absent.  PB_CFG1's reset value is 20h, so bit 5 of the field is
 * already set and DIS_DC_BLOCKER (bit 6) must survive untouched. */
ZTEST(alp_chips, test_tas2563_set_amp_level_writes_field_and_rejects_unlisted_codes)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	zassert_equal(fake_tas2563_get_reg(TAS_REG_PB_CFG1), 0x20u, "POR value, 10h << 1");

	/* Quietest listed level: 01h << 1 = 02h, everything else preserved. */
	zassert_equal(tas2563_set_amp_level(&ctx, TAS2563_AMP_LEVEL_MIN), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PB_CFG1), 0x02u, "AMP_LEVEL = 01h in bits 5..1");

	/* DIS_DC_BLOCKER (bit 6) is not ours; a level change must not move it. */
	fake_tas2563_set_reg(TAS_REG_PB_CFG1, 0x42u);
	zassert_equal(tas2563_set_amp_level(&ctx, TAS2563_AMP_LEVEL_MAX), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PB_CFG1),
	              0x78u,
	              "1Ch << 1 = 38h, with DIS_DC_BLOCKER (bit 6) preserved");

	const uint32_t before = fake_tas2563_write_count(TAS_REG_PB_CFG1);
	zassert_equal(tas2563_set_amp_level(&ctx, 0x00u), ALP_ERR_OUT_OF_RANGE, "00h is unlisted");
	zassert_equal(tas2563_set_amp_level(&ctx, 0x1Du), ALP_ERR_OUT_OF_RANGE, "1Dh is Reserved");
	zassert_equal(tas2563_set_amp_level(&ctx, 0x1Fu), ALP_ERR_OUT_OF_RANGE, "1Fh is Reserved");
	zassert_equal(tas2563_set_amp_level(&ctx, 0xFFu), ALP_ERR_OUT_OF_RANGE, "wider than 5 bits");
	zassert_equal(fake_tas2563_write_count(TAS_REG_PB_CFG1),
	              before,
	              "a refused level must not reach the bus");

	alp_i2c_close(bus);
}

/* TDM_CFG0/1/2 field placement -- SLASET3D 7.5.8 Table 7-108 p.69,
 * 7.5.9 Table 7-109 p.69, 7.5.10 Table 7-110 p.70. */
ZTEST(alp_chips, test_tas2563_configure_i2s_writes_tdm_fields)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	/* 16 kHz, 16-bit, standard I2S, right channel.
     *   TDM_CFG0 = (09h & ~0Eh) | (001b << 1) = 03h
     *   TDM_CFG1 = (02h & ~7Eh) | (1    << 1) = 02h  (I2S: 1 SBCLK offset)
     *   TDM_CFG2 = (4Ah & ~3Fh) | (10b<<4)|(00b<<2)|00b = 60h
     *              (IVMON_LEN in bits 7..6 preserved as 01b) */
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.sample_rate_hz   = 16000u;
	cfg.word_bits        = 16u;
	cfg.format           = ALP_I2S_FMT_I2S;
	zassert_equal(tas2563_configure_i2s(&ctx, &cfg, TAS2563_RX_RIGHT), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG0), 0x03u, "SAMP_RATE in bits 3..1");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG1), 0x02u, "RX_OFFSET = 1 for I2S");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG2),
	              0x60u,
	              "RX_SCFG=10b (right), RX_WLEN=00b, RX_SLEN=00b, IVMON_LEN untouched");

	/* 96 kHz, 32-bit, left-justified, left channel.
     *   TDM_CFG0 = (03h & ~0Eh) | (101b << 1) = 0Bh
     *   TDM_CFG1 = (02h & ~7Eh) | (0    << 1) = 00h  (no offset)
     *   TDM_CFG2 = (60h & ~3Fh) | (01b<<4)|(11b<<2)|10b = 5Eh */
	cfg.sample_rate_hz = 96000u;
	cfg.word_bits      = 32u;
	cfg.format         = ALP_I2S_FMT_LEFT_JUSTIFIED;
	zassert_equal(tas2563_configure_i2s(&ctx, &cfg, TAS2563_RX_LEFT), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG0), 0x0Bu);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG1),
	              0x00u,
	              "left-justified drops the one-SBCLK I2S offset");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG2), 0x5Eu);

	/* 44.1 kHz, 24-bit, I2S, stereo downmix -- the width that pins
     * RX_WLEN=10b against RX_SLEN=01b (both 24-bit, but different
     * encodings, so a slot/word mix-up shows here and nowhere else).
     *   TDM_CFG0 = (0Bh & ~0Eh) | (100b << 1) = 09h
     *   TDM_CFG1 = (00h & ~7Eh) | (1    << 1) = 02h
     *   TDM_CFG2 = (5Eh & ~3Fh) | (11b<<4)|(10b<<2)|01b = 79h */
	cfg.sample_rate_hz = 44100u;
	cfg.word_bits      = 24u;
	cfg.format         = ALP_I2S_FMT_I2S;
	zassert_equal(tas2563_configure_i2s(&ctx, &cfg, TAS2563_RX_DOWNMIX), ALP_OK);
	zassert_equal(
	    fake_tas2563_get_reg(TAS_REG_TDM_CFG0), 0x09u, "44.1 and 48 kHz share the 100b encoding");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG1), 0x02u);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG2),
	              0x79u,
	              "RX_WLEN=10b (24-bit word) with RX_SLEN=01b (24-bit slot)");

	/* 8 kHz, 16-bit, right-justified, slot from I2C address.
     * Right-justified is the RX_JUSTIFY bit, not an offset.
     *   TDM_CFG0 = (09h & ~0Eh) | (000b << 1) = 01h
     *   TDM_CFG1 = (02h & ~7Eh) | 40h         = 40h
     *   TDM_CFG2 = (79h & ~3Fh) | 0            = 40h */
	cfg.sample_rate_hz = 8000u;
	cfg.word_bits      = 16u;
	cfg.format         = ALP_I2S_FMT_RIGHT_JUSTIFIED;
	zassert_equal(tas2563_configure_i2s(&ctx, &cfg, TAS2563_RX_SLOT_FROM_ADDR), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG0), 0x01u);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG1), 0x40u, "RX_JUSTIFY set, RX_OFFSET zero");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG2), 0x40u);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tas2563_configure_i2s_rejects_unencodable_configs)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0);
	cfg.sample_rate_hz   = 48000u;
	cfg.word_bits        = 16u;
	cfg.format           = ALP_I2S_FMT_I2S;

	zassert_equal(tas2563_configure_i2s(&ctx, NULL, TAS2563_RX_LEFT), ALP_ERR_INVAL);
	zassert_equal(tas2563_configure_i2s(&ctx, &cfg, (tas2563_rx_channel_t)4), ALP_ERR_INVAL);

	alp_i2s_config_t bad = cfg;
	bad.sample_rate_hz   = 44101u; /* Not in Table 7-108. */
	zassert_equal(tas2563_configure_i2s(&ctx, &bad, TAS2563_RX_LEFT), ALP_ERR_OUT_OF_RANGE);

	bad           = cfg;
	bad.word_bits = 12u; /* Not in Table 7-110. */
	zassert_equal(tas2563_configure_i2s(&ctx, &bad, TAS2563_RX_LEFT), ALP_ERR_OUT_OF_RANGE);

	/* 20-bit IS in Table 7-110 but is deliberately unmapped: word_bits
     * is documented 16/24/32, so no host bus can be opened at 20 and
     * the mapping would be unreachable through the real API. */
	bad           = cfg;
	bad.word_bits = 20u;
	zassert_equal(tas2563_configure_i2s(&ctx, &bad, TAS2563_RX_LEFT), ALP_ERR_OUT_OF_RANGE);

	/* TDM_CFG1 has no short-/long-frame-sync PCM encoding. */
	bad        = cfg;
	bad.format = ALP_I2S_FMT_PCM_SHORT;
	zassert_equal(tas2563_configure_i2s(&ctx, &bad, TAS2563_RX_LEFT), ALP_ERR_NOSUPPORT);

	bad        = cfg;
	bad.format = ALP_I2S_FMT_PCM_LONG;
	zassert_equal(tas2563_configure_i2s(&ctx, &bad, TAS2563_RX_LEFT), ALP_ERR_NOSUPPORT);

	zassert_equal(fake_tas2563_write_count(TAS_REG_TDM_CFG0),
	              0u,
	              "every rejection must happen before any TDM register is written");

	alp_i2c_close(bus);
}

/* IV sense needs the sense blocks powered (PWR_CTL bits 3..2, reset
 * HIGH = powered down, Table 7-104 p.66) AND a transmit slot each
 * (TDM_CFG5/TDM_CFG6, Tables 7-113/7-114 p.71).  The ORDER matters:
 * power up first on enable, stop transmitting first on disable. */
ZTEST(alp_chips, test_tas2563_configure_iv_sense_orders_power_before_slots)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	zassert_equal(tas2563_configure_iv_sense(&ctx, true, 2u, 0u), ALP_OK);
	zassert_equal(
	    fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x02u, "ISNS_PD/VSNS_PD cleared, MODE untouched");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG5), 0x42u, "VSNS_TX set, slot 2");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG6), 0x40u, "ISNS_TX set, slot 0");

	/* Ordering, read off the write log. */
	size_t pwr_at = SIZE_MAX, slot_at = SIZE_MAX;
	for (size_t i = 0; i < fake_tas2563_log_len(); ++i) {
		const struct fake_tas2563_write *w = fake_tas2563_log(i);
		if (w->reg == TAS_REG_PWR_CTL && pwr_at == SIZE_MAX) pwr_at = i;
		if (w->reg == TAS_REG_TDM_CFG5 && slot_at == SIZE_MAX) slot_at = i;
	}
	zassert_not_equal(pwr_at, SIZE_MAX);
	zassert_not_equal(slot_at, SIZE_MAX);
	zassert_true(pwr_at < slot_at,
	             "a transmit slot must never be enabled against a powered-down sense block");

	/* Teardown runs the other way: slots off, then power down. */
	fake_tas2563_log_reset();
	zassert_equal(tas2563_configure_iv_sense(&ctx, false, 0u, 0u), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG5),
	              0x02u,
	              "VSNS_TX cleared, the slot number left alone");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_TDM_CFG6), 0x00u);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x0Eu, "both sense blocks powered down");

	pwr_at  = SIZE_MAX;
	slot_at = SIZE_MAX;
	for (size_t i = 0; i < fake_tas2563_log_len(); ++i) {
		const struct fake_tas2563_write *w = fake_tas2563_log(i);
		if (w->reg == TAS_REG_PWR_CTL && pwr_at == SIZE_MAX) pwr_at = i;
		if (w->reg == TAS_REG_TDM_CFG5 && slot_at == SIZE_MAX) slot_at = i;
	}
	zassert_true(slot_at < pwr_at, "stop transmitting before powering the sense blocks down");

	/* The slot fields are 6 bits wide (Tables 7-113/7-114, p.71). */
	zassert_equal(tas2563_configure_iv_sense(&ctx, true, 64u, 0u), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(tas2563_configure_iv_sense(&ctx, true, 0u, 64u), ALP_ERR_OUT_OF_RANGE);

	alp_i2c_close(bus);
}

/* IRQZ_PU is MISC_CFG1 bit 3 (Table 7-106, p.68); IRQZ_PIN_CFG is
 * INT & CLK CFG bits 1..0 and 01b means "assert on unmasked LATCHED
 * interrupts" (Table 7-143, p.86).  INT & CLK CFG is pre-seeded to
 * 1Bh (PIN_CFG = 11b) so the write to 01b is observable rather than
 * matching the reset value. */
ZTEST(alp_chips, test_tas2563_configure_fault_pin_sets_pullup_and_latched_assert)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);
	fake_tas2563_set_reg(TAS_REG_INT_CLK, 0x1Bu);

	alp_gpio_t *irq = alp_gpio_open(TAS_PIN_IRQ_N);
	zassert_not_null(irq);

	zassert_equal(tas2563_configure_fault_pin(&ctx, NULL, true), ALP_ERR_INVAL);

	zassert_equal(tas2563_configure_fault_pin(&ctx, irq, true), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_MISC_CFG1),
	              0xCEu,
	              "IRQZ_PU (bit 3) set on top of the C6h reset value");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_INT_CLK),
	              0x19u,
	              "IRQZ_PIN_CFG = 01b, reserved bits 5..3 (3h) preserved");

	zassert_equal(tas2563_configure_fault_pin(&ctx, irq, false), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_MISC_CFG1),
	              0xC6u,
	              "the internal pull-up must be switchable back off");

	alp_gpio_close(irq);
	alp_i2c_close(bus);
}

/* IRQ_N is open drain, active low (7.3.12 Figure 7-10 p.36,
 * Table 7-13 p.37): a LOW pin is a fault. */
ZTEST(alp_chips, test_tas2563_fault_asserted_reads_active_low)
{
	tas2563_t  ctx;
	alp_i2c_t *bus      = tas_init(&ctx, 0x0Eu, NULL);
	bool       asserted = true;

	zassert_equal(
	    tas2563_fault_asserted(&ctx, &asserted), ALP_ERR_NOSUPPORT, "no fault pin bound yet");

	alp_gpio_t *irq = alp_gpio_open(TAS_PIN_IRQ_N);
	zassert_not_null(irq);
	zassert_equal(tas2563_configure_fault_pin(&ctx, irq, true), ALP_OK);

	zassert_equal(tas2563_fault_asserted(&ctx, NULL), ALP_ERR_INVAL);

	zassert_ok(gpio_emul_input_set(tas_gpio_dev(), TAS_PIN_IRQ_N, 1));
	zassert_equal(tas2563_fault_asserted(&ctx, &asserted), ALP_OK);
	zassert_false(asserted, "pin released high = no fault");

	zassert_ok(gpio_emul_input_set(tas_gpio_dev(), TAS_PIN_IRQ_N, 0));
	zassert_equal(tas2563_fault_asserted(&ctx, &asserted), ALP_OK);
	zassert_true(asserted, "pin pulled low = fault");

	alp_gpio_close(irq);
	alp_i2c_close(bus);
}

/* INT_LTCH0/1/3/4 (24h/25h/26h/27h) are packed byte-aligned into bits
 * 0..7 / 8..15 / 16..23 / 24..31, so each TAS2563_FAULT_* macro lands
 * where its datasheet table puts it (Tables 7-136..7-139, p.82-85). */
ZTEST(alp_chips, test_tas2563_read_faults_packs_latched_registers_byte_aligned)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	fake_tas2563_set_reg(TAS_REG_INT_LTCH0, 0x03u); /* over temp + over current */
	fake_tas2563_set_reg(TAS_REG_INT_LTCH1, 0x20u); /* load diagnostics complete */
	fake_tas2563_set_reg(TAS_REG_INT_LTCH3, 0x80u); /* DAC MOD clock error */
	fake_tas2563_set_reg(TAS_REG_INT_LTCH4, 0x08u); /* ASI2 clock error */

	uint32_t faults = 0;
	zassert_equal(tas2563_read_faults(&ctx, &faults), ALP_OK);
	zassert_equal(faults, 0x08802003u, "LTCH0/1/3/4 must land in bytes 0/1/2/3, in that order");
	zassert_true((faults & TAS2563_FAULT_OVER_TEMP) != 0u);
	zassert_true((faults & TAS2563_FAULT_OVER_CURRENT) != 0u);
	zassert_true((faults & TAS2563_FAULT_LOAD_DIAG_DONE) != 0u);
	zassert_true((faults & TAS2563_FAULT_DAC_MOD_CLOCK) != 0u);
	zassert_true((faults & TAS2563_FAULT_ASI2_CLOCK) != 0u);
	zassert_equal(faults & TAS2563_FAULT_SHUTDOWN_CAUSES,
	              TAS2563_FAULT_OVER_TEMP | TAS2563_FAULT_OVER_CURRENT,
	              "only the datasheet's self-shutdown causes belong in that mask");

	zassert_equal(tas2563_read_faults(&ctx, NULL), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

/* CLR_INTP_LTCH is INT & CLK CFG bit 2 and is self clearing
 * (Table 7-143 p.86, Table 7-11 p.37). */
ZTEST(alp_chips, test_tas2563_clear_faults_sets_the_self_clearing_bit)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	fake_tas2563_set_reg(TAS_REG_INT_LTCH0, 0xFFu);
	fake_tas2563_set_reg(TAS_REG_INT_LTCH4, 0x88u);

	zassert_equal(tas2563_clear_faults(&ctx), ALP_OK);

	uint32_t faults = 0xDEADBEEFu;
	zassert_equal(tas2563_read_faults(&ctx, &faults), ALP_OK);
	zassert_equal(faults, 0u, "every latched register must have been cleared");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_INT_CLK),
	              0x19u,
	              "the rest of INT & CLK CFG, incl. reserved bits 5..3, is preserved");

	alp_i2c_close(bus);
}

/* The device map is book/page paged and BOOK (7Fh) is only reachable
 * from page 0 (7.3.10 p.34, 7.5.62 p.94).  Pin the exact write
 * sequence, including the paging writes and the restore to book 0 /
 * page 0 that every other driver function depends on. */
ZTEST(alp_chips, test_tas2563_load_tuning_replays_records_with_correct_paging)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, NULL);

	const tas2563_tuning_reg_t records[] = {
		{ .book = 0u, .page = 0u, .reg = 0x10u, .val = 0xAAu },
		{ .book = 0u, .page = 1u, .reg = 0x20u, .val = 0xBBu },
		{ .book = 8u, .page = 0u, .reg = 0x30u, .val = 0xCCu },
		{ .book = 8u, .page = 3u, .reg = 0x31u, .val = 0xDDu },
	};
	zassert_equal(tas2563_load_tuning(&ctx, records, ARRAY_SIZE(records), NULL), ALP_OK);

	/* The book/page columns are the selection IN FORCE when the write
     * went out, so a PAGE write shows the page it is leaving. */
	static const struct fake_tas2563_write expected[] = {
		{ 0u, 0u, 0x00u, 0x00u },                           /* PAGE = 0, known state */
		{ 0u, 0u, 0x7Fu, 0x00u },                           /* BOOK = 0, known state */
		{ 0u, 0u, 0x10u, 0xAAu }, { 0u, 0u, 0x00u, 0x01u }, /* PAGE = 1 */
		{ 0u, 1u, 0x20u, 0xBBu }, { 0u, 1u, 0x00u, 0x00u }, /* PAGE = 0 before touching BOOK */
		{ 0u, 0u, 0x7Fu, 0x08u },                           /* BOOK = 8 */
		{ 8u, 0u, 0x30u, 0xCCu },                           /* no redundant PAGE write: a book
									 switch already lands on page 0 */
		{ 8u, 0u, 0x00u, 0x03u },                           /* PAGE = 3 */
		{ 8u, 3u, 0x31u, 0xDDu }, { 8u, 3u, 0x00u, 0x00u }, /* restore PAGE = 0 */
		{ 8u, 0u, 0x7Fu, 0x00u },                           /* restore BOOK = 0 */
	};
	zassert_equal(fake_tas2563_log_len(), ARRAY_SIZE(expected), "unexpected write count");
	for (size_t i = 0; i < ARRAY_SIZE(expected); ++i) {
		const struct fake_tas2563_write *w = fake_tas2563_log(i);
		zassert_equal(w->book, expected[i].book, "log[%zu].book", i);
		zassert_equal(w->page, expected[i].page, "log[%zu].page", i);
		zassert_equal(w->reg, expected[i].reg, "log[%zu].reg", i);
		zassert_equal(w->val, expected[i].val, "log[%zu].val", i);
	}
	zassert_equal(fake_tas2563_cur_book(), 0u);
	zassert_equal(fake_tas2563_cur_page(), 0u);
	zassert_equal(fake_tas2563_get_reg(0x10u), 0xAAu);

	alp_i2c_close(bus);
}

/* A blob may not steer the paging registers, and it may not write
 * PWR_CTL -- that would let a tuning file change the amplifier's
 * operating mode, including bringing it out of shutdown.  Validation
 * runs over the whole stream BEFORE any write, so a bad record does
 * not leave a half-applied tuning behind. */
ZTEST(alp_chips, test_tas2563_load_tuning_rejects_illegal_records_before_writing)
{
	tas2563_t  ctx;
	alp_i2c_t *bus    = tas_init(&ctx, 0x0Eu, NULL);
	size_t     failed = SIZE_MAX;

	const tas2563_tuning_reg_t page_reg[] = { { 0u, 0u, 0x00u, 0x01u } };
	zassert_equal(tas2563_load_tuning(&ctx, page_reg, 1u, &failed), ALP_ERR_INVAL);
	zassert_equal(failed, 0u);

	const tas2563_tuning_reg_t book_reg[] = { { 0u, 0u, 0x7Fu, 0x01u } };
	zassert_equal(tas2563_load_tuning(&ctx, book_reg, 1u, &failed), ALP_ERR_INVAL);

	const tas2563_tuning_reg_t with_pwr_ctl[] = {
		{ 0u, 0u, 0x10u, 0xAAu },
		{ 0u, 0u, TAS_REG_PWR_CTL, 0x00u }, /* MODE -> ACTIVE.  Refused. */
	};
	failed = SIZE_MAX;
	zassert_equal(tas2563_load_tuning(&ctx, with_pwr_ctl, 2u, &failed),
	              ALP_ERR_INVAL,
	              "a tuning blob must not be able to write PWR_CTL");
	zassert_equal(failed, 1u, "the offending record index is reported");
	zassert_equal(fake_tas2563_log_len(), 0u, "nothing may be written when a record is illegal");
	fake_tas2563_log_reset();

	/* SW_RESET, MISC (IRQZ_POL) and TG_CFG0 join PWR_CTL on the
     * blocklist: each changes something the caller believes it
     * configured, with no readback saying so. */
	const tas2563_tuning_reg_t reserved[] = {
		{ 0u, 0u, 0x01u, 0x01u }, /* SW_RESET  -- wipes the caller's config */
		{ 0u, 0u, 0x32u, 0x00u }, /* MISC      -- IRQZ_POL inverts the fault pin */
		{ 0u, 0u, 0x3Fu, 0xC0u }, /* TG_CFG0   -- arms the tone generator */
	};
	for (size_t i = 0; i < ARRAY_SIZE(reserved); ++i) {
		failed = SIZE_MAX;
		zassert_equal(tas2563_load_tuning(&ctx, &reserved[i], 1u, &failed),
		              ALP_ERR_INVAL,
		              "reg 0x%02x must be refused in book 0 / page 0",
		              reserved[i].reg);
		zassert_equal(failed, 0u);
		/* Same register in another book is a coefficient, not ours. */
		tas2563_tuning_reg_t elsewhere = reserved[i];
		elsewhere.book                 = 3u;
		zassert_equal(tas2563_load_tuning(&ctx, &elsewhere, 1u, NULL),
		              ALP_OK,
		              "the blocklist is book-0/page-0 only");
	}

	/* PB_CFG1 (AMP_LEVEL) is deliberately NOT blocked -- output level
     * is what a smart-amp tuning is for. */
	const tas2563_tuning_reg_t amp_level[] = { { 0u, 0u, TAS_REG_PB_CFG1, 0x3Au } };
	zassert_equal(tas2563_load_tuning(&ctx, amp_level, 1u, NULL),
	              ALP_OK,
	              "a tuning must still be able to set the output level");
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PB_CFG1), 0x3Au);

	zassert_equal(tas2563_load_tuning(&ctx, NULL, 3u, NULL), ALP_ERR_INVAL);
	zassert_equal(tas2563_load_tuning(&ctx, NULL, 0u, NULL), ALP_OK, "empty stream is a no-op");

	alp_i2c_close(bus);
}

/* A NACK partway through must still leave the device on book 0 /
 * page 0: every other function in this driver addresses that page and
 * would otherwise read and write coefficients instead of control
 * registers. */
ZTEST(alp_chips, test_tas2563_load_tuning_restores_paging_after_a_bus_failure)
{
	tas2563_t  ctx;
	alp_i2c_t *bus    = tas_init(&ctx, 0x0Eu, NULL);
	size_t     failed = SIZE_MAX;

	const tas2563_tuning_reg_t records[] = {
		{ 0u, 1u, 0x20u, 0xBBu },
		{ 0u, 1u, 0x21u, 0xCCu },
	};
	fake_tas2563_fail_write_at(0u, 1u, 0x21u);

	zassert_not_equal(tas2563_load_tuning(&ctx, records, ARRAY_SIZE(records), &failed),
	                  ALP_OK,
	                  "a NACKed register write must surface");
	zassert_equal(failed, 1u, "the record that failed is reported");
	zassert_equal(fake_tas2563_cur_page(), 0u, "paging restored to page 0 on the failure path");
	zassert_equal(fake_tas2563_cur_book(), 0u, "paging restored to book 0 on the failure path");

	alp_i2c_close(bus);
}

/* With no SD_N line there is no hardware shutdown to fall back on, so
 * deinit writes software shutdown instead of leaving a possibly
 * ACTIVE Class-D stage behind. */
ZTEST(alp_chips, test_tas2563_deinit_software_shuts_down_when_no_sd_n)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Cu, NULL);

	zassert_equal(tas2563_set_mode(&ctx, TAS2563_MODE_ACTIVE), ALP_OK);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL), 0x0Cu);

	tas2563_deinit(&ctx);
	zassert_equal(fake_tas2563_get_reg(TAS_REG_PWR_CTL),
	              0x0Eu,
	              "deinit without SD_N must leave MODE = 10b (software shutdown)");

	alp_i2c_close(bus);
}

/* With SD_N owned, deinit drops the pin -- hardware shutdown, which
 * needs no bus traffic (7.3.11.1, p.34). */
ZTEST(alp_chips, test_tas2563_deinit_drops_sd_n_when_owned)
{
	alp_gpio_t *sd_n = alp_gpio_open(TAS_PIN_SD_N);
	zassert_not_null(sd_n);

	tas2563_t  ctx;
	alp_i2c_t *bus = tas_init(&ctx, 0x0Eu, sd_n);
	zassert_equal(
	    gpio_emul_output_get(tas_gpio_dev(), TAS_PIN_SD_N), 1, "init releases hardware shutdown");

	tas2563_deinit(&ctx);
	zassert_equal(gpio_emul_output_get(tas_gpio_dev(), TAS_PIN_SD_N), 0, "deinit asserts SD_N low");

	alp_gpio_close(sd_n);
	alp_i2c_close(bus);
}

/* #2077: when tas2563_init() owns sd_n it waits TAS2563_RESET_SETTLE_US
 * TWICE on this path -- once after driving SDZ high (hardware reset)
 * and once after its own unconditional software reset -- before its
 * first I2C access (SLASET3D §7.3.11.1 "I2C communication is disabled"
 * in Hardware Shutdown, §9.2's 100 us OTP-load floor, which applies to
 * both reset kinds).  Asserting >= 2x the floor, not just >= 1x, is
 * what makes this test able to catch EITHER wait being dropped alone --
 * at >= 1x a regression that drops one of the two would still pass.
 *
 * fake_tas2563.c's i2c-emul target does not model bus timing (the same
 * limitation test_bmi323_init_honours_suspend_mode_communication_idle
 * above documents for fake_bmi323.c), so this cannot assert either
 * wait's POSITION relative to a bus access -- only that tas2563_init()'s
 * real wall-clock time is at least the combined floor.  That is still
 * enough to catch a wait being dropped: nothing else on this path takes
 * measurable time. */
ZTEST(alp_chips, test_tas2563_init_settles_sdz_before_first_access_when_sd_n_owned)
{
	fake_tas2563_reset();
	alp_gpio_t *sd_n = alp_gpio_open(TAS_PIN_SD_N);
	zassert_not_null(sd_n);

	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);

	/* k_uptime_ticks() is tick-resolution (too coarse to see a 200 us
	 * spin land inside one system tick); k_cycle_get_32() tracks the
	 * same HW cycle counter k_busy_wait() itself spins against, so it
	 * actually resolves a sub-tick busy-wait. */
	tas2563_t ctx;
	uint32_t  t0 = k_cycle_get_32();
	zassert_equal(tas2563_init(&ctx, bus, TAS_FAKE_ADDR, sd_n), ALP_OK);
	uint64_t elapsed_us = k_cyc_to_us_floor64(k_cycle_get_32() - t0);

	zassert_true(elapsed_us >= 2u * TAS2563_RESET_SETTLE_US,
	             "tas2563_init() with sd_n owned took %llu us, want >= %u us "
	             "(2x TAS2563_RESET_SETTLE_US: hardware-reset settle + "
	             "software-reset settle) -- looks like one of the two waits "
	             "was dropped",
	             (unsigned long long)elapsed_us,
	             2u * TAS2563_RESET_SETTLE_US);

	alp_gpio_close(sd_n);
	alp_i2c_close(bus);
}

#endif /* DT_NODE_EXISTS(DT_NODELABEL(fake_tas2563)) */

/* ------------------------------------------------------------------ */
/* v0.5 §D.audio batch -- NULL-arg guard smokes                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ics_43434_init_null_args)
{
	ics_43434_t dev;
	zassert_equal(ics_43434_init(NULL, ICS_43434_CH_LEFT), ALP_ERR_INVAL);
	zassert_equal(ics_43434_init(&dev, (ics_43434_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_inmp441_init_null_args)
{
	inmp441_t dev;
	zassert_equal(inmp441_init(NULL, INMP441_CH_LEFT), ALP_ERR_INVAL);
	zassert_equal(inmp441_init(&dev, (inmp441_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_wm8960_init_null_args)
{
	wm8960_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(wm8960_init(NULL, bus, WM8960_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(wm8960_init(&dev, NULL, WM8960_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(wm8960_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tlv320aic3204_init_null_args)
{
	tlv320aic3204_t dev;
	alp_i2c_t      *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(tlv320aic3204_init(NULL, bus, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(tlv320aic3204_init(&dev, NULL, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(tlv320aic3204_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_max98357a_init_null_args)
{
	max98357a_t dev;
	zassert_equal(max98357a_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(max98357a_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_es8388_init_null_args)
{
	es8388_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(es8388_init(NULL, bus, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(es8388_init(&dev, NULL, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(es8388_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}
