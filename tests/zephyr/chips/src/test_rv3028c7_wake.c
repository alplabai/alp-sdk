/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * RV-3028-C7 wake-source services (countdown timer, alarm, flag
 * service) against fake_rv3028c7.c.  The ordered write log is what
 * proves the Application Manual Rev. 1.4 Sec. 4.8.2 start sequence and
 * that no EEPROM command or EEPROM-mirror register is ever touched.
 */

#include <zephyr/ztest.h>

#include "alp/chips/rv3028c7.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"
#include "fakes.h"

#define R_ALARM_MIN 0x07u
#define R_ALARM_HR  0x08u
#define R_ALARM_WD  0x09u
#define R_TVAL0     0x0Au
#define R_TVAL1     0x0Bu
#define R_TSTAT0    0x0Cu
#define R_TSTAT1    0x0Du
#define R_STATUS    0x0Eu
#define R_CTRL1     0x0Fu
#define R_CTRL2     0x10u

static rv3028c7_t g_ctx;
static alp_i2c_t *g_bus;

static void wake_end(void)
{
	if (g_bus != NULL) {
		rv3028c7_deinit(&g_ctx);
		alp_i2c_close(g_bus);
		g_bus = NULL;
	}
}

static void wake_begin(void)
{
	/* Each ZTEST re-opens the bus; close the previous handle so the
	 * static handle pool is not exhausted for the suites that follow. */
	wake_end();
	fake_rv3028c7_reset();
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	g_bus = bus;
	zassert_equal(rv3028c7_init(&g_ctx, bus), ALP_OK);
	fake_rv3028c7_wlog_reset();
}

/* No EEPROM command, EEPROM handshake register, or EEPROM-mirror
 * register (0x25..0x27, 0x30..0x37) in the write log, and the EEPROM
 * backing store is untouched. */
static void assert_no_eeprom_access(void)
{
	for (size_t i = 0; i < fake_rv3028c7_wlog_len(); i++) {
		uint8_t r = fake_rv3028c7_wlog_reg(i);
		zassert_false((r >= 0x25u && r <= 0x27u) || (r >= 0x30u && r <= 0x37u),
		              "EEPROM-related reg 0x%02x written (entry %u)",
		              r,
		              (unsigned)i);
	}
	for (unsigned a = 0; a < 256; a++) {
		zassert_equal(fake_rv3028c7_get_eeprom((uint8_t)a), 0u);
	}
}

static int wlog_find(uint8_t reg, int from)
{
	for (size_t i = (size_t)from; i < fake_rv3028c7_wlog_len(); i++) {
		if (fake_rv3028c7_wlog_reg(i) == reg) return (int)i;
	}
	return -1;
}

ZTEST(alp_chips, test_rv3028c7_timer_start_1hz_sequence)
{
	wake_begin();
	/* Preserve unrelated bits: WADA + USEL + EERD in CONTROL_1,
	 * UIE + 12_24 in CONTROL_2. */
	fake_rv3028c7_set_reg(R_CTRL1, 0x38u);
	fake_rv3028c7_set_reg(R_CTRL2, 0x22u);
	fake_rv3028c7_set_reg(R_STATUS, 0x01u | 0x08u); /* PORF + stale TF */

	uint32_t actual = 0;
	zassert_equal(rv3028c7_timer_start(&g_ctx, 90, &actual), ALP_OK);
	zassert_equal(actual, 90u);

	/* TD = 10 (1 Hz), TRPT = 0, TE = 1; WADA/USEL kept. */
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1), 0x3Eu);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL2), 0x32u); /* UIE + 12_24 kept, TIE set */
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 90u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL1), 0u);
	/* TF cleared, PORF preserved. */
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u);

	/* Ordered sequence (Sec. 4.8.2): TE off, TIE off, TF clear, TD
	 * write, value, TIE on, TE on last. */
	zassert_equal(fake_rv3028c7_wlog_len(), 8u);
	zassert_equal(fake_rv3028c7_wlog_reg(0), R_CTRL1);
	zassert_equal(fake_rv3028c7_wlog_val(0), 0x38u);
	zassert_equal(fake_rv3028c7_wlog_reg(1), R_CTRL2);
	zassert_equal(fake_rv3028c7_wlog_val(1) & 0x10u, 0u);
	zassert_equal(fake_rv3028c7_wlog_reg(2), R_STATUS);
	zassert_equal(fake_rv3028c7_wlog_val(2) & 0x08u, 0u);
	zassert_equal(fake_rv3028c7_wlog_reg(3), R_CTRL1);
	zassert_equal(fake_rv3028c7_wlog_val(3), 0x3Au); /* EERD kept, TE still 0 */
	zassert_equal(fake_rv3028c7_wlog_reg(4), R_TVAL0);
	zassert_equal(fake_rv3028c7_wlog_reg(5), R_TVAL1);
	zassert_equal(fake_rv3028c7_wlog_reg(6), R_CTRL2);
	zassert_equal(fake_rv3028c7_wlog_val(6) & 0x10u, 0x10u);
	zassert_equal(fake_rv3028c7_wlog_reg(7), R_CTRL1);
	zassert_equal(fake_rv3028c7_wlog_val(7), 0x3Eu);

	assert_no_eeprom_access();
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_timer_td_selection_boundaries)
{
	uint32_t actual;

	/* 1 s and 4095 s stay on 1 Hz (TD = 10). */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 1, &actual), ALP_OK);
	zassert_equal(actual, 1u);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1) & 0x03u, 0x02u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 1u);

	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 4095, &actual), ALP_OK);
	zassert_equal(actual, 4095u);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1) & 0x03u, 0x02u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 0xFFu);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL1), 0x0Fu);

	/* 4096 s moves to 1/60 Hz (TD = 11), rounded UP to 69 min = 4140 s. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 4096, &actual), ALP_OK);
	zassert_equal(actual, 4140u);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1) & 0x03u, 0x03u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 69u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL1), 0u);

	/* Exact multiple of 60 is not rounded. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 7200, &actual), ALP_OK);
	zassert_equal(actual, 7200u);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 120u);

	/* Maximum: 4095 min. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, RV3028C7_TIMER_MAX_SECONDS, &actual), ALP_OK);
	zassert_equal(actual, RV3028C7_TIMER_MAX_SECONDS);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 0xFFu);
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL1), 0x0Fu);
	assert_no_eeprom_access();

	/* Out of range: rejected with no bus traffic. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 0, NULL), ALP_ERR_INVAL);
	zassert_equal(rv3028c7_timer_start(&g_ctx, RV3028C7_TIMER_MAX_SECONDS + 1u, NULL),
	              ALP_ERR_INVAL);
	zassert_equal(fake_rv3028c7_wlog_len(), 0u);
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_timer_stop_and_read)
{
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 100, NULL), ALP_OK);

	/* Hardware counts down: live status 0x0042 = 66 s left of 100. */
	fake_rv3028c7_set_reg(R_TSTAT0, 0x42u);
	fake_rv3028c7_set_reg(R_TSTAT1, 0x00u);
	rv3028c7_timer_state_t st;
	zassert_equal(rv3028c7_timer_read(&g_ctx, &st), ALP_OK);
	zassert_true(st.running);
	zassert_false(st.expired);
	zassert_equal(st.preset_ms, 100000u);
	zassert_equal(st.remaining_ms, 66000u);
	zassert_equal(st.elapsed_ms, 34000u);

	/* 1/60 Hz scaling: preset 3 min, 1 left. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 4200, NULL), ALP_OK); /* 70 min */
	fake_rv3028c7_set_reg(R_TSTAT0, 0x01u);
	zassert_equal(rv3028c7_timer_read(&g_ctx, &st), ALP_OK);
	zassert_equal(st.preset_ms, 70u * 60000u);
	zassert_equal(st.remaining_ms, 60000u);
	zassert_equal(st.elapsed_ms, 69u * 60000u);

	/* Expiry: TF latched, hardware cleared TE. */
	fake_rv3028c7_set_reg(R_STATUS, 0x08u);
	fake_rv3028c7_set_reg(R_CTRL1, fake_rv3028c7_get_reg(R_CTRL1) & (uint8_t)~0x04u);
	fake_rv3028c7_set_reg(R_TSTAT0, 0x00u);
	zassert_equal(rv3028c7_timer_read(&g_ctx, &st), ALP_OK);
	zassert_false(st.running);
	zassert_true(st.expired);
	zassert_equal(st.remaining_ms, 0u);

	/* Stop: TE, TIE, TF all cleared; preset not zeroed. */
	wake_begin();
	zassert_equal(rv3028c7_timer_start(&g_ctx, 5, NULL), ALP_OK);
	fake_rv3028c7_set_reg(R_STATUS, 0x08u | 0x01u);
	fake_rv3028c7_wlog_reset();
	zassert_equal(rv3028c7_timer_stop(&g_ctx), ALP_OK);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1) & 0x04u, 0u);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL2) & 0x10u, 0u);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u); /* PORF kept */
	zassert_equal(fake_rv3028c7_get_reg(R_TVAL0), 5u);
	zassert_equal(wlog_find(R_TVAL0, 0), -1);
	zassert_equal(wlog_find(R_TVAL1, 0), -1);
	assert_no_eeprom_access();
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_alarm_arm_and_clear)
{
	wake_begin();
	fake_rv3028c7_set_reg(R_STATUS, 0x04u | 0x01u); /* stale AF + PORF */

	rv3028c7_time_t        when  = { .minute = 30, .hour = 7, .day = 15 };
	rv3028c7_alarm_match_t match = {
		.match_minute = true, .match_hour = true, .match_day_or_weekday = true, .use_weekday = false
	};
	zassert_equal(rv3028c7_alarm_arm(&g_ctx, &when, &match), ALP_OK);

	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_MIN), 0x30u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_HR), 0x07u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_WD), 0x15u);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL1) & 0x20u, 0x20u); /* WADA = date */
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL2) & 0x08u, 0x08u); /* AIE */
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u);        /* AF cleared, PORF kept */
	/* Ordering: AIE off first, then AF cleared, before the compare
	 * registers are touched. */
	zassert_equal(fake_rv3028c7_wlog_reg(0), R_CTRL2);
	zassert_equal(fake_rv3028c7_wlog_val(0) & 0x08u, 0u);
	zassert_equal(fake_rv3028c7_wlog_reg(1), R_STATUS);
	/* AIE is the last write: enabled only after the compare is loaded. */
	zassert_equal(fake_rv3028c7_wlog_reg(fake_rv3028c7_wlog_len() - 1), R_CTRL2);
	assert_no_eeprom_access();

	/* Minutes-only match: hour and date AE bits set (field ignored). */
	match.match_hour           = false;
	match.match_day_or_weekday = false;
	zassert_equal(rv3028c7_alarm_arm(&g_ctx, &when, &match), ALP_OK);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_MIN), 0x30u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_HR), 0x87u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_WD), 0x95u);

	/* Clear: AIE off, all AE = 1, AF cleared, PORF kept. */
	fake_rv3028c7_set_reg(R_STATUS, 0x04u | 0x01u);
	fake_rv3028c7_wlog_reset();
	zassert_equal(rv3028c7_alarm_clear(&g_ctx), ALP_OK);
	zassert_equal(fake_rv3028c7_get_reg(R_CTRL2) & 0x08u, 0u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_MIN), 0x80u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_HR), 0x80u);
	zassert_equal(fake_rv3028c7_get_reg(R_ALARM_WD), 0x80u);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u);
	assert_no_eeprom_access();
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_wake_service_clears_tf_af_uf_keeps_porf)
{
	wake_begin();
	uint8_t flags = 0xFFu;
	fake_rv3028c7_set_reg(R_CTRL2, 0x10u | 0x08u | 0x20u); /* TIE + AIE + UIE */

	/* Nothing pending: reported 0, no write issued. */
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, 0u);
	zassert_equal(fake_rv3028c7_wlog_len(), 0u);

	/* TF + PORF + EVF + BSF latched: only TF is acknowledged. */
	fake_rv3028c7_set_reg(R_STATUS, 0x08u | 0x01u | 0x02u | 0x20u);
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, RV3028C7_WAKE_TF);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u | 0x02u | 0x20u);
	zassert_equal(fake_rv3028c7_wlog_len(), 1u);
	zassert_equal(fake_rv3028c7_wlog_reg(0), R_STATUS);
	zassert_equal(fake_rv3028c7_wlog_val(0), 0x7Fu & (uint8_t)~0x08u);

	/* TF + AF + UF together, PORF still preserved, NULL out is allowed. */
	fake_rv3028c7_set_reg(R_STATUS, 0x08u | 0x04u | 0x10u | 0x01u);
	zassert_equal(rv3028c7_wake_service(&g_ctx, NULL), ALP_OK);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u);
	assert_no_eeprom_access();
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_wake_service_reports_only_enabled_sources)
{
	wake_begin();
	uint8_t flags = 0xFFu;

	/* UF latches every second regardless of UIE: with UIE = 0 it is not
	 * a wake cause, but it is still cleared. */
	fake_rv3028c7_set_reg(R_CTRL2, 0x00u);
	fake_rv3028c7_set_reg(R_STATUS, 0x10u | 0x01u);
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, 0u);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x01u);

	/* TF + AF + UF latched, only TIE enabled: just TF is reported. */
	fake_rv3028c7_set_reg(R_CTRL2, 0x10u);
	fake_rv3028c7_set_reg(R_STATUS, 0x08u | 0x04u | 0x10u);
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, RV3028C7_WAKE_TF);
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0u);

	/* UIE set: UF is reported. */
	fake_rv3028c7_set_reg(R_CTRL2, 0x20u);
	fake_rv3028c7_set_reg(R_STATUS, 0x10u);
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, RV3028C7_WAKE_UF);
	wake_end();
}

/* What would break if writing 1 to a STATUS flag SET it instead of being
 * ignored (the assumption behind the constant-mask acknowledge, see
 * RV3028_STATUS_WRITE1_IGNORED in rv3028c7.c): the mask's 1s would
 * latch every other flag.  This pins the failure mode so a bench result
 * of "write 1 sets" is visible here; if the helper is switched to the
 * read-back strategy, this test is the one to invert. */
ZTEST(alp_chips, test_rv3028c7_wake_service_if_write1_sets_latches_spurious_flags)
{
	wake_begin();
	fake_rv3028c7_set_write1_sets(true);
	fake_rv3028c7_set_reg(R_CTRL2, 0x10u);
	fake_rv3028c7_set_reg(R_STATUS, 0x08u);
	uint8_t flags = 0;
	zassert_equal(rv3028c7_wake_service(&g_ctx, &flags), ALP_OK);
	zassert_equal(flags, RV3028C7_WAKE_TF);
	/* TF is gone, but every other flag in the 0x77 mask is now set. */
	zassert_equal(fake_rv3028c7_get_reg(R_STATUS), 0x77u);
	wake_end();
}

ZTEST(alp_chips, test_rv3028c7_wake_calls_reject_bad_state)
{
	rv3028c7_t             ctx = { 0 };
	rv3028c7_timer_state_t st;
	rv3028c7_time_t        when  = { .year = 2026, .month = 5, .day = 13 };
	rv3028c7_alarm_match_t match = { .match_minute = true };
	uint8_t                f;

	zassert_equal(rv3028c7_timer_start(&ctx, 1, NULL), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_timer_stop(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_timer_read(&ctx, &st), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_alarm_arm(&ctx, &when, &match), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_alarm_clear(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(rv3028c7_wake_service(&ctx, &f), ALP_ERR_NOT_READY);

	wake_begin();
	zassert_equal(rv3028c7_timer_read(&g_ctx, NULL), ALP_ERR_INVAL);
	zassert_equal(rv3028c7_alarm_arm(&g_ctx, NULL, &match), ALP_ERR_INVAL);
	wake_end();
}
