/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SoM power-domain runtime (#2784, U5) on native_sim.
 *
 * GPIO and I2C emulators stand in for the AEN LPGPIO / GPIO / BRD_I2C blocks
 * (app.overlay), the CC3501E driver is faked, and the real som_power*.c files
 * run against them.  Covered:
 *   - policy validation: RAIL_OFF refused without its Kconfig gate, NOT_PRESENT
 *     for a domain the SKU does not carry (EXT_RAM here), KEEP_ALIVE honoured;
 *   - quiesce order (consumers first) and the reverse restore;
 *   - the default pin / I2C actions and their restore levels;
 *   - the CC3501E restore is cc3501e_hard_reset(), never cc3501e_reset();
 *   - the BKRAM record: CRC, and a boot with an invalid record touches nothing;
 *   - a boot with a valid record restores and is reported;
 *   - failure of one domain rolls back the others.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zephyr/drivers/flash/flash_ospi_alif.h>

#include <alp/chips/tmp112.h>
#include <alp/power.h>

#include "fakes.h"
#include "som_power.h"
#include "som_power_chips.h"

static uint32_t g_image = 0xA1111111u; /* the "running image" */
static unsigned g_stat_clears;
static unsigned g_stat_clear_order, g_i2c_decode_order_seen;

uint32_t alp_som_pd_image_id(void)
{
	return g_image;
}

#define LPGPIO DEVICE_DT_GET(DT_NODELABEL(lpgpio))
#define GPIO11 DEVICE_DT_GET(DT_NODELABEL(gpio11))
#define GPIO5  DEVICE_DT_GET(DT_NODELABEL(gpio5))

#define EMUL_TMP DT_NODELABEL(tmp112)
#define EMUL_RTC DT_NODELABEL(rv3028)

#define NRST_PIN  1
#define WIFIEN    5
#define PHY_PWR   4
#define FLASH_RST 7
#define PHY_RST   6
#define BL_PIN    5

#define RTC_CLKOUT_REG 0x35u
#define RTC_CTRL1_REG  0x0Fu
#define RTC_EERD       0x08u

static void pin_high(const struct device *port, gpio_pin_t pin)
{
	zassert_ok(gpio_pin_configure(port, pin, GPIO_OUTPUT_HIGH));
}

static int level(const struct device *port, gpio_pin_t pin)
{
	return gpio_emul_output_get(port, pin);
}

static uint8_t *tmp_regs(void)
{
	return fake_regs(EMUL_DT_GET(EMUL_TMP));
}

static uint8_t *rtc_regs(void)
{
	return fake_regs(EMUL_DT_GET(EMUL_RTC));
}

static uint16_t tmp_conf(void)
{
	return (uint16_t)(((uint16_t)tmp_regs()[1] << 8) | tmp_regs()[2]);
}

static void before(void *unused)
{
	(void)unused;
	g_image = 0xA1111111u;
	alp_som_pd_bench_set(0u); /* the cell belongs to this image: a previous test must not leak */
	alp_som_power_reset_for_test();
	fakes_reset();

	/* The running board: nRESET released, WIFI_EN on, PHY powered and out of
	 * reset, NOR out of reset, backlight on. */
	pin_high(LPGPIO, NRST_PIN);
	pin_high(LPGPIO, WIFIEN);
	pin_high(LPGPIO, PHY_PWR);
	pin_high(LPGPIO, FLASH_RST);
	pin_high(GPIO11, PHY_RST);
	/* The Alif GPIO reads the pad level back on an output; the emulator only
	 * does so for a pin that is configured as both. */
	zassert_ok(gpio_pin_configure(GPIO5, BL_PIN, GPIO_OUTPUT_HIGH | GPIO_INPUT));

	memset(tmp_regs(), 0, 256);
	tmp_regs()[1] = 0x60u; /* CONF = 0x60A0, continuous */
	tmp_regs()[2] = 0xA0u;
	memset(rtc_regs(), 0, 256);
	rtc_regs()[RTC_CLKOUT_REG] = 0x80u; /* CLKOE set, FD = 0 (32.768 kHz) */
}

ZTEST_SUITE(power_som_domains, NULL, NULL, before, NULL, NULL);

/* ---- Policy and info --------------------------------------------------------- */

ZTEST(power_som_domains, test_info_describes_populated_domains)
{
	alp_power_domain_info_t i;

	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_WIFI_BLE, &i));
	zassert_true(i.present);
	zassert_equal(i.default_action, ALP_POWER_ACTION_HOLD_RESET);
	zassert_equal(i.dependents, ALP_POWER_DEP_CAM_LDO);
	zassert_false(i.holds_through_stop, "LPGPIO hold through STOP is unproven");
#ifdef ALP_TEST_RAIL_OFF
	zassert_true((i.supported_actions & ALP_POWER_ACTION_RAIL_OFF) != 0u);
#else
	zassert_true((i.supported_actions & ALP_POWER_ACTION_RAIL_OFF) == 0u);
#endif

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_ETH_PHY, &i));
	zassert_equal(i.default_action, ALP_POWER_ACTION_POWERDOWN_PIN);
	zassert_equal(i.dependents, ALP_POWER_DEP_PHY_REFCLK, "Y3 oscillator shares P15_4");

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_EXT_FLASH, &i));
	zassert_equal(i.default_action, ALP_POWER_ACTION_HOLD_RESET, "no flash DPD hook exists");

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_TEMP_SENSOR, &i));
	zassert_equal(i.default_action, ALP_POWER_ACTION_SHUTDOWN_REG);

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_RTC, &i));
	zassert_true(i.present);
	zassert_equal(i.default_action, ALP_POWER_ACTION_NONE, "the RTC is never powered down");

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_BACKLIGHT, &i));
	zassert_equal(i.default_action, ALP_POWER_ACTION_POWERDOWN_PIN);
}

ZTEST(power_som_domains, test_absent_domain_is_not_present)
{
	alp_power_domain_info_t i;

	memset(&i, 0, sizeof(i));
	zassert_ok(alp_som_power_ops_domain_info(ALP_POWER_DOMAIN_EXT_RAM, &i));
	zassert_false(i.present);
	zassert_equal(i.supported_actions, 0u);
	zassert_equal(alp_som_power_ops_policy_set(
	                  NULL, ALP_POWER_DOMAIN_EXT_RAM, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(power_som_domains, test_rail_off_is_gated_by_kconfig)
{
	alp_status_t s = alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_RAIL_OFF);
#ifdef ALP_TEST_RAIL_OFF
	zassert_equal(s, ALP_OK);
	zassert_equal(alp_som_power_policy(ALP_POWER_DOMAIN_WIFI_BLE),
	              ALP_POWER_DOMAIN_POLICY_RAIL_OFF);
#else
	zassert_equal(s, ALP_ERR_NOSUPPORT, "RAIL_OFF needs CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF");
	zassert_equal(alp_som_power_policy(ALP_POWER_DOMAIN_WIFI_BLE), ALP_POWER_DOMAIN_POLICY_AUTO);
#endif
	/* No other domain carries a rail-off action, with or without the gate. */
	zassert_equal(alp_som_power_ops_policy_set(
	                  NULL, ALP_POWER_DOMAIN_ETH_PHY, ALP_POWER_DOMAIN_POLICY_RAIL_OFF),
	              ALP_ERR_NOSUPPORT);
	zassert_equal(
	    alp_som_power_ops_policy_set(NULL, ALP_POWER_DOMAIN_RTC, ALP_POWER_DOMAIN_POLICY_RAIL_OFF),
	    ALP_ERR_NOSUPPORT);
}

/* ---- Quiesce order ------------------------------------------------------------ */

static alp_power_domain_t g_log[16];
static bool               g_log_restore[16];
static size_t             g_log_n;
static alp_power_domain_t g_fail_on         = ALP_POWER_DOMAIN_COUNT;
static alp_power_domain_t g_fail_restore_on = ALP_POWER_DOMAIN_COUNT;

static alp_status_t log_quiesce(void *ctx, bool rail_off)
{
	(void)rail_off;
	alp_power_domain_t d     = (alp_power_domain_t)(uintptr_t)ctx;
	g_log[g_log_n]           = d;
	g_log_restore[g_log_n++] = false;
	return (d == g_fail_on) ? ALP_ERR_IO : ALP_OK;
}

static alp_status_t log_restore(void *ctx, bool rail_off, bool early)
{
	(void)rail_off;
	(void)early;
	alp_power_domain_t d     = (alp_power_domain_t)(uintptr_t)ctx;
	g_log[g_log_n]           = d;
	g_log_restore[g_log_n++] = true;
	return (d == g_fail_restore_on) ? ALP_ERR_IO : ALP_OK;
}

static const alp_som_power_hooks_t log_hooks = { .quiesce = log_quiesce, .restore = log_restore };

static void bind_all_logging(void)
{
	g_log_n           = 0;
	g_fail_on         = ALP_POWER_DOMAIN_COUNT;
	g_fail_restore_on = ALP_POWER_DOMAIN_COUNT;
	for (int d = 0; d < (int)ALP_POWER_DOMAIN_COUNT; ++d) {
		alp_power_domain_info_t i;
		(void)alp_som_power_ops_domain_info((alp_power_domain_t)d, &i);
		if (i.present) {
			zassert_ok(alp_som_power_bind((alp_power_domain_t)d, &log_hooks, (void *)(uintptr_t)d));
		}
	}
}

ZTEST(power_som_domains, test_same_boot_rollback_does_not_need_the_bkram_record)
{
	uint32_t failed = 99u;

	bind_all_logging();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_not_equal(alp_som_power_quiesced(), 0u);

	/* The SE call (or anything else) took BKRAM away: the record no longer loads. */
	alp_som_pd_store_clear();
	alp_som_pd_record_t rec;

	zassert_false(alp_som_pd_store_load(&rec));

	zassert_ok(alp_som_power_restore(&failed), "unwound from the in-RAM state");
	zassert_equal(failed, 0u);
	zassert_equal(alp_som_power_quiesced(), 0u, "every domain is active again");
	zassert_equal(g_log_n, 12u, "6 quiesced, 6 restored");

	/* Nothing quiesced and nothing recorded: still NOT_READY, not a blind restore. */
	zassert_equal(alp_som_power_restore(NULL), ALP_ERR_NOT_READY);
}

ZTEST(power_som_domains, test_quiesce_consumers_first_restore_in_reverse)
{
	static const alp_power_domain_t want[] = {
		ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_EXT_FLASH, ALP_POWER_DOMAIN_TEMP_SENSOR,
		ALP_POWER_DOMAIN_RTC,      ALP_POWER_DOMAIN_BACKLIGHT, ALP_POWER_DOMAIN_ETH_PHY,
	};
	uint32_t failed = 99u;

	bind_all_logging();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_equal(g_log_n, ARRAY_SIZE(want));
	for (size_t i = 0; i < ARRAY_SIZE(want); ++i) {
		zassert_equal(g_log[i], want[i], "quiesce slot %zu", i);
		zassert_false(g_log_restore[i]);
	}
	zassert_equal(alp_som_power_quiesced(),
	              ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_WIFI_BLE) |
	                  ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_EXT_FLASH) |
	                  ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_TEMP_SENSOR) |
	                  ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_RTC) |
	                  ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_BACKLIGHT) |
	                  ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_ETH_PHY));

	zassert_ok(alp_som_power_restore(&failed));
	zassert_equal(failed, 0u);
	zassert_equal(g_log_n, 2u * ARRAY_SIZE(want));
	for (size_t i = 0; i < ARRAY_SIZE(want); ++i) {
		size_t at = ARRAY_SIZE(want) + i;
		zassert_true(g_log_restore[at]);
		zassert_equal(g_log[at], want[ARRAY_SIZE(want) - 1u - i], "restore slot %zu", i);
	}
	zassert_equal(alp_som_power_quiesced(), 0u);
}

ZTEST(power_som_domains, test_failed_quiesce_rolls_back)
{
	bind_all_logging();
	g_fail_on = ALP_POWER_DOMAIN_RTC; /* fourth in order */

	uint32_t rb = 99u;

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_STOP, &rb), ALP_ERR_IO);
	zassert_equal(rb, 0u, "every rollback step succeeded");
	/* wifi, flash, temp quiesced, rtc failed -> rtc's own partial step AND the
	 * three come back, newest first. */
	zassert_equal(g_log_n, 8u);
	zassert_equal(g_log[4], ALP_POWER_DOMAIN_RTC, "the failing domain is rolled back too");
	zassert_true(g_log_restore[4]);
	zassert_equal(g_log[5], ALP_POWER_DOMAIN_TEMP_SENSOR);
	zassert_equal(g_log[6], ALP_POWER_DOMAIN_EXT_FLASH);
	zassert_equal(g_log[7], ALP_POWER_DOMAIN_WIFI_BLE);
	zassert_equal(alp_som_power_quiesced(), 0u);
	zassert_equal(alp_som_power_restore(NULL), ALP_ERR_NOT_READY, "no record was written");
}

ZTEST(power_som_domains, test_failure_on_the_first_domain_rolls_back_only_itself)
{
	bind_all_logging();
	g_fail_on = ALP_POWER_DOMAIN_WIFI_BLE;

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL), ALP_ERR_IO);
	zassert_equal(g_log_n, 2u);
	zassert_equal(g_log[1], ALP_POWER_DOMAIN_WIFI_BLE);
	zassert_true(g_log_restore[1]);
}

ZTEST(power_som_domains, test_rollback_failure_is_reported_and_retryable)
{
	uint32_t rb = 0u;

	bind_all_logging();
	g_fail_on         = ALP_POWER_DOMAIN_RTC;
	g_fail_restore_on = ALP_POWER_DOMAIN_EXT_FLASH;

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_STOP, &rb), ALP_ERR_IO);
	zassert_equal(rb,
	              ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_EXT_FLASH),
	              "the rollback failure reaches the caller");
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_EXT_FLASH), ALP_SOM_PD_RESTORE_FAILED);

	/* The retry record is a RUN record whatever mode failed: after a warm reset
	 * STOP_MODE_STAT reads 0 and would drop a STOP record, leaving the domain held. */
	alp_som_pd_record_t rec;

	zassert_true(alp_som_pd_store_load(&rec));
	zassert_equal(rec.mode, (uint32_t)ALP_POWER_MODE_RUN);
	zassert_equal(rec.quiesced, ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_EXT_FLASH));

	/* It stays in the record, so a later restore retries just that domain. */
	g_fail_restore_on = ALP_POWER_DOMAIN_COUNT;
	g_log_n           = 0;
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_log_n, 1u);
	zassert_equal(g_log[0], ALP_POWER_DOMAIN_EXT_FLASH);
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_EXT_FLASH), ALP_SOM_PD_ACTIVE);
}

ZTEST(power_som_domains, test_rollback_retry_record_survives_a_warm_reset)
{
	alp_power_boot_info_t info;
	alp_som_pd_record_t   rec;
	uint32_t              rb = 0u;

	bind_all_logging();
	g_fail_on         = ALP_POWER_DOMAIN_RTC;
	g_fail_restore_on = ALP_POWER_DOMAIN_EXT_FLASH;
	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_STOP, &rb), ALP_ERR_IO);
	zassert_not_equal(rb, 0u);
	zassert_true(alp_som_pd_store_load(&rec));

	/* A warm reset: RAM state gone, STOP_MODE_STAT = 0.  A STOP-mode record would
	 * be dropped here and the NOR left in reset. */
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid, "the retry record is a RUN record");
	zassert_equal(info.restored_domains, ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_EXT_FLASH));
}

ZTEST(power_som_domains, test_keep_alive_policy_is_never_touched)
{
	bind_all_logging();
	zassert_ok(alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_BACKLIGHT, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	for (size_t i = 0; i < g_log_n; ++i) {
		zassert_not_equal(g_log[i], ALP_POWER_DOMAIN_BACKLIGHT);
	}
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_BACKLIGHT), ALP_SOM_PD_ACTIVE);
	zassert_ok(alp_som_power_restore(NULL));
}

ZTEST(power_som_domains, test_sleep_modes_do_not_quiesce)
{
	bind_all_logging();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_SLEEP, NULL));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_DEEP_SLEEP, NULL));
	zassert_equal(g_log_n, 0u);
	zassert_equal(alp_som_power_quiesced(), 0u);
	zassert_equal(alp_som_power_quiesce((alp_power_mode_t)99, NULL), ALP_ERR_INVAL);

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STANDBY, NULL));
	zassert_equal(g_log_n, 6u, "STANDBY quiesces like STOP");
	zassert_ok(alp_som_power_restore(NULL));
}

/* ---- Default pin / I2C actions -------------------------------------------------- */

ZTEST(power_som_domains, test_default_actions_and_restore_levels)
{
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));

	zassert_equal(level(LPGPIO, NRST_PIN), 0, "CC3501E nRESET held low");
	zassert_equal(level(LPGPIO, WIFIEN), 1, "AUTO never touches WIFI_EN");
	zassert_equal(level(LPGPIO, PHY_PWR), 0, "E_PHY_PWRDWN low (also gates the Y3 oscillator)");
	zassert_equal(level(LPGPIO, FLASH_RST), 0, "OSPI1_RESETn held low");
	zassert_equal(level(GPIO5, BL_PIN), 0, "backlight EN low");
	zassert_true((tmp_conf() & 0x0100u) != 0u, "TMP112 CONF.SD set");
	zassert_equal(tmp_conf() & ~0x0100u, 0x60A0u, "other CONF bits kept");
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x87u, "CLKOUT_FD = low, CLKOE untouched");
	zassert_true((rtc_regs()[RTC_CTRL1_REG] & RTC_EERD) != 0u,
	             "EERD set: the 24 h EEPROM refresh must not turn CLKOUT back on");
	zassert_equal(g_pads_calls, 1u, "pads muxed once, before the first drive");
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_ETH_PHY),
	              ALP_SOM_PD_QUIESCED,
	              "PHY state is tracked in software");

	uint32_t failed = 7u;
	zassert_ok(alp_som_power_restore(&failed));
	zassert_equal(failed, 0u);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
	zassert_equal(level(LPGPIO, PHY_PWR), 1);
	zassert_equal(level(GPIO11, PHY_RST), 1, "PHY reset pulse ends released");
	zassert_equal(level(LPGPIO, FLASH_RST), 1);
	zassert_equal(level(GPIO5, BL_PIN), 1);
	zassert_equal(tmp_conf() & 0x0100u, 0u, "TMP112 SD cleared");
	zassert_equal(rtc_regs()[RTC_CTRL1_REG] & RTC_EERD, 0u, "EERD cleared on wake");
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_ETH_PHY), ALP_SOM_PD_ACTIVE);
}

ZTEST(power_som_domains, test_rtc_eerd_goes_back_to_its_pre_quiesce_value)
{
	/* Already paused by someone else (e.g. an EEPROM write in progress): stays so. */
	rtc_regs()[RTC_CTRL1_REG] |= RTC_EERD;
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_true((rtc_regs()[RTC_CTRL1_REG] & RTC_EERD) != 0u);
	zassert_ok(alp_som_power_restore(NULL));
	zassert_true((rtc_regs()[RTC_CTRL1_REG] & RTC_EERD) != 0u, "not unconditionally cleared");

	/* Not paused before: cleared again. */
	rtc_regs()[RTC_CTRL1_REG] &= (uint8_t)~RTC_EERD;
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(rtc_regs()[RTC_CTRL1_REG] & RTC_EERD, 0u);
}

ZTEST(power_som_domains, test_backlight_that_was_off_stays_off)
{
	zassert_ok(gpio_pin_configure(GPIO5, BL_PIN, GPIO_OUTPUT_LOW | GPIO_INPUT));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(level(GPIO5, BL_PIN), 0);
}

ZTEST(power_som_domains, test_cycle_run_hook)
{
	uint32_t failed = 5u;

	zassert_ok(alp_som_power_cycle_run(60, &failed));
	zassert_equal(failed, 0u);
	zassert_equal(alp_som_power_quiesced(), 0u);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
}

/* ---- CC3501E ----------------------------------------------------------------- */

ZTEST(power_som_domains, test_cc3501e_restore_is_hard_reset_never_reset)
{
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_true(g_cc.nrst_written);
	zassert_false(g_cc.nrst_level, "nRESET held low");
	zassert_false(g_cc.en_written, "AUTO leaves WIFI_EN alone");
	zassert_false(g_fw.initialised, "the context reads as down while held");
	zassert_equal(g_cc.power_off_calls, 0u);

	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_cc.hard_reset_calls, 1u);
	zassert_equal(g_cc.reset_calls, 0u, "cc3501e_reset() drops WIFI_EN: never on restore");
	zassert_false(g_cc.en_written, "no WIFI_EN toggle");
	zassert_true(g_fw.initialised, "context re-armed after the restore");
}

ZTEST(power_som_domains, test_cc3501e_uninitialised_context_stays_down)
{
	g_fw.initialised = false;
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_false(g_fw.initialised, "restore does not initialise what was never up");
}

#ifdef ALP_TEST_RAIL_OFF
ZTEST(power_som_domains, test_cc3501e_rail_off_uses_power_off_then_hard_reset)
{
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));
	zassert_ok(alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_RAIL_OFF));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_equal(g_cc.power_off_calls, 1u);

	zassert_ok(alp_som_power_restore(NULL));
	zassert_true(g_cc.en_written && g_cc.en_level, "WIFI_EN raised before the nRESET release");
	zassert_equal(g_cc.hard_reset_calls, 1u);
	zassert_equal(g_cc.reset_calls, 0u);
	zassert_true(g_fw.initialised);
}

ZTEST(power_som_domains, test_rail_off_default_path_gates_wifi_en)
{
	zassert_ok(alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_RAIL_OFF));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(level(LPGPIO, NRST_PIN), 0, "nRESET low before the supply drops");
	zassert_equal(level(LPGPIO, WIFIEN), 0);
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(level(LPGPIO, WIFIEN), 1);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
}
#endif

/* ---- BKRAM record and the cold-boot restore ---------------------------------------- */

/* Everything a quiesce changes and a restore puts back (the RTC's CLKOUT is
 * deliberately left off by the restore, so it is checked separately). */
static void assert_pins_untouched(void)
{
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
	zassert_equal(level(LPGPIO, PHY_PWR), 1);
	zassert_equal(level(LPGPIO, FLASH_RST), 1);
	zassert_equal(level(GPIO5, BL_PIN), 1);
	zassert_equal(tmp_conf(), 0x60A0u);
}

ZTEST(power_som_domains, test_record_crc_detects_corruption)
{
	alp_som_pd_record_t r = { .mode = 4u, .quiesced = 0x3fu, .prior_active = 0x3fu };

	alp_som_pd_store_save(&r);
	alp_som_pd_record_t back;
	zassert_true(alp_som_pd_store_load(&back));
	zassert_equal(back.quiesced, 0x3fu);

	back.quiesced ^= 1u;
	alp_som_pd_store_poke(&back);
	zassert_false(alp_som_pd_store_load(&back), "a flipped bit fails the CRC");
}

ZTEST(power_som_domains, test_boot_with_invalid_record_touches_nothing)
{
	g_stop_mode = 0u; /* a plain POR: STOP_MODE_STAT clear */
	alp_power_boot_info_t info;

	/* 1. Empty store: a plain POR. */
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid);
	assert_pins_untouched();

	/* 2. Right shape, wrong magic. */
	alp_som_pd_record_t r = { .magic = 0xdeadbeefu, .mode = 4u, .quiesced = 0x7fu };
	r.crc                 = alp_som_pd_record_crc(&r);
	alp_som_pd_store_poke(&r);
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid);
	assert_pins_untouched();

	/* 3. Right magic, wrong CRC. */
	r.magic = ALP_SOM_PD_RECORD_MAGIC;
	r.crc ^= 0x5au;
	alp_som_pd_store_poke(&r);
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid);
	zassert_equal(info.quiesced_domains, 0u);
	zassert_equal(info.restored_domains, 0u);
	assert_pins_untouched();
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x80u);
	zassert_equal(g_cc.hard_reset_calls, 0u);
}

ZTEST(power_som_domains, test_boot_with_valid_record_restores_and_reports)
{
	alp_power_boot_info_t info;

	/* Quiesce for STOP, then "lose" the in-RAM state exactly as the cold boot
	 * does: the record is all that survives. */
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	uint32_t held = alp_som_power_quiesced();
	zassert_not_equal(held, 0u);
	zassert_equal(level(LPGPIO, NRST_PIN), 0);

	alp_som_pd_record_t rec;
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);

	/* Pass 1 (POST_KERNEL 0): pin domains only.  The I2C controller is not up
	 * yet, so the TMP112 / RTC stay held until pass 2. */
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(info.quiesced_domains, held);
	uint32_t i2c_bits = ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_TEMP_SENSOR) |
	                    ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_RTC);
	zassert_equal(info.restored_domains, held & ~i2c_bits);
	zassert_not_equal(tmp_conf() & 0x0100u, 0u, "TMP112 not touched before the I2C pass");
	zassert_equal(level(LPGPIO, NRST_PIN), 1);

	/* Pass 2 (after the I2C controller): the rest. */
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_equal(info.restored_domains, held);
	zassert_equal(info.restore_failed_domains, 0u);
	assert_pins_untouched();
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x87u, "restore leaves the RTC CLKOUT off");
	zassert_equal(rtc_regs()[RTC_CTRL1_REG] & RTC_EERD, 0u);

	/* The record is consumed: a second boot pass is a plain POR. */
	alp_som_pd_record_t gone;
	zassert_false(alp_som_pd_store_load(&gone));
}

ZTEST(power_som_domains, test_i2c_pass_without_record_does_nothing)
{
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	assert_pins_untouched();
}

/* STOP_MODE_STAT bit 4 must agree that this boot is a STOP wake. */
ZTEST(power_som_domains, test_boot_stop_mode_stat_gate)
{
	alp_power_boot_info_t info;
	alp_som_pd_record_t   rec;

	/* STAT = 0: the record is discarded and nothing is touched -- the domains stay
	 * as the (unrelated) reset left them. */
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid);
	zassert_equal(level(LPGPIO, NRST_PIN), 0, "nothing was restored");
	zassert_not_equal(tmp_conf() & 0x0100u, 0u);
	zassert_false(alp_som_pd_store_load(&rec), "the record is dropped");

	/* STAT = 1 (bit 4): restored. */
	before(NULL);
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0x10u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);

	/* A RUN-mode record (an interrupted bench cycle) does not need the bit. */
	before(NULL);
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
}

ZTEST(power_som_domains, test_restore_without_record_touches_nothing)
{
	zassert_equal(alp_som_power_restore(NULL), ALP_ERR_NOT_READY);
	assert_pins_untouched();
}

/* ---- TMP112 chip driver shutdown ---------------------------------------------------- */

ZTEST(power_som_domains, test_tmp112_chip_shutdown_and_bind)
{
	tmp112_t ctx = { .initialised = true, .bus = (alp_i2c_t *)&g_chip_regs, .addr = 0x48u };

	g_chip_regs[1] = 0x60u;
	g_chip_regs[2] = 0xA0u;

	zassert_equal(tmp112_set_shutdown(NULL, true), ALP_ERR_NOT_READY);
	zassert_ok(tmp112_set_shutdown(&ctx, true));
	zassert_equal(((uint16_t)g_chip_regs[1] << 8) | g_chip_regs[2], 0x61A0u);
	zassert_ok(tmp112_set_shutdown(&ctx, false));
	zassert_equal(((uint16_t)g_chip_regs[1] << 8) | g_chip_regs[2], 0x60A0u);

	/* Through the domain layer the chip driver owns the bit, not the DT I2C path. */
	zassert_ok(alp_som_power_bind_tmp112(&ctx));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(((uint16_t)g_chip_regs[1] << 8) | g_chip_regs[2], 0x61A0u);
	zassert_equal(tmp_conf(), 0x60A0u, "the DT I2C path was bypassed");
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(((uint16_t)g_chip_regs[1] << 8) | g_chip_regs[2], 0x60A0u);
}

ZTEST(power_som_domains, test_bind_rejects_bad_arguments)
{
	zassert_equal(alp_som_power_bind(ALP_POWER_DOMAIN_COUNT, &log_hooks, NULL), ALP_ERR_INVAL);
	zassert_equal(alp_som_power_bind(ALP_POWER_DOMAIN_RTC, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(alp_som_power_bind_cc3501e(NULL), ALP_ERR_INVAL);
	zassert_equal(alp_som_power_bind_tmp112(NULL), ALP_ERR_INVAL);
	zassert_equal(alp_som_power_bind_rv3028(NULL), ALP_ERR_INVAL);

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(alp_som_power_bind(ALP_POWER_DOMAIN_RTC, &log_hooks, NULL),
	              ALP_ERR_BUSY,
	              "no re-binding while a domain is held");
	zassert_equal(alp_som_power_ops_policy_set(
	                  NULL, ALP_POWER_DOMAIN_RTC, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE),
	              ALP_ERR_BUSY);
	zassert_ok(alp_som_power_restore(NULL));
}

ZTEST(power_som_domains, test_rv3028_hook_routes_clkout_low_and_pauses_refresh)
{
	rv3028c7_t ctx = { .bus = (alp_i2c_t *)&g_chip_regs };

	zassert_ok(alp_som_power_bind_rv3028(&ctx));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(g_clkout_calls, 1u);
	zassert_equal(g_clkout_src, (int)RV3028C7_CLKOUT_LOW);
	zassert_true((g_chip_regs[RTC_CTRL1_REG] & RTC_EERD) != 0u, "EERD set after the route");
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x80u, "the DT I2C path was bypassed");
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_clkout_calls, 1u, "restore does not re-route CLKOUT");
	zassert_equal(g_chip_regs[RTC_CTRL1_REG] & RTC_EERD, 0u, "EERD cleared on wake");

	/* Paused before the quiesce: the hook restores THAT, not zero. */
	g_chip_regs[RTC_CTRL1_REG] = RTC_EERD;
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_true((g_chip_regs[RTC_CTRL1_REG] & RTC_EERD) != 0u);
}

/* ---- CC3501E link handshake ------------------------------------------------------------ */

ZTEST(power_som_domains, test_cc3501e_restore_pings_before_rearming)
{
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_true(g_cc.ping_calls >= 1u, "the blind hard reset is followed by a PING");
	zassert_true(g_fw.initialised);
}

ZTEST(power_som_domains, test_cc3501e_that_never_answers_stays_down)
{
	uint32_t failed = 0u;

	g_cc.ping_always_fail = true;
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	zassert_equal(alp_som_power_restore(&failed), ALP_ERR_IO);
	zassert_equal(failed, ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_WIFI_BLE));
	zassert_false(g_fw.initialised, "no re-arm without a handshake");
	zassert_equal(g_cc.reset_calls, 0u);
}

/* ---- Pad mux ------------------------------------------------------------------------------ */

ZTEST(power_som_domains, test_pads_are_applied_before_the_first_drive_and_failure_refuses)
{
	g_pads_rc = ALP_ERR_NOT_READY;
	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL),
	              ALP_ERR_NOT_READY,
	              "an unmuxed pad is never reported as driven");
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "nothing was driven");
	zassert_equal(level(LPGPIO, PHY_PWR), 1);
	zassert_equal(alp_som_power_quiesced(), 0u);

	g_pads_rc = ALP_OK; /* it retries until the state applies */
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(level(LPGPIO, NRST_PIN), 0);
	zassert_ok(alp_som_power_restore(NULL));
}

ZTEST(power_som_domains, test_a_hold_that_does_not_read_back_fails_the_quiesce)
{
	/* nRESET (pin 1) is the first pad driven: it reads high although driven low. */
	g_pad_stuck_pin = NRST_PIN;
	uint32_t rb     = 0u;

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_RUN, &rb),
	              ALP_ERR_IO,
	              "the layer must not report a hold it cannot see");
	zassert_equal(alp_som_power_quiesced(), 0u);
	/* The stuck pad cannot confirm the release either, so the rollback is reported. */
	zassert_equal(rb, ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_WIFI_BLE));
	zassert_equal(level(LPGPIO, PHY_PWR), 1, "no later domain was touched");

	g_pad_stuck_pin = -1; /* the pad recovers; the kept record retries the release */
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
}

/* ---- OSPI NOR: no reset on top of a transfer ----------------------------------------------------- */

static struct {
	unsigned int suspends;
	unsigned int resumes;
	int          suspend_rc;
	int          level_at_suspend;
	int          level_at_resume;
	bool         locked;
} g_nor;

int flash_ospi_alif_suspend(const struct device *dev, uint32_t timeout_ms)
{
	(void)dev;
	(void)timeout_ms;
	g_nor.suspends++;
	g_nor.level_at_suspend = level(LPGPIO, FLASH_RST);
	if (g_nor.suspend_rc != 0) {
		return g_nor.suspend_rc; /* a write holds the lock: not taken */
	}
	g_nor.locked = true;
	return 0;
}

int flash_ospi_alif_resume(const struct device *dev)
{
	(void)dev;
	g_nor.resumes++;
	g_nor.level_at_resume = level(LPGPIO, FLASH_RST);
	g_nor.locked          = false;
	return 0;
}

ZTEST(power_som_domains, test_nor_is_locked_before_reset_and_unlocked_after_release)
{
	memset(&g_nor, 0, sizeof(g_nor));
	zassert_ok(alp_som_power_bind_flash(DEVICE_DT_GET(EMUL_TMP)));

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL));
	zassert_equal(g_nor.suspends, 1u);
	zassert_equal(g_nor.level_at_suspend, 1, "RESETn still released when the lock is taken");
	zassert_true(g_nor.locked, "no access can start while RESETn is held");
	zassert_equal(level(LPGPIO, FLASH_RST), 0);

	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_nor.resumes, 1u);
	zassert_equal(g_nor.level_at_resume, 1, "RESETn released before the driver unlocks");
	zassert_false(g_nor.locked);
}

ZTEST(power_som_domains, test_nor_write_in_flight_blocks_the_quiesce)
{
	memset(&g_nor, 0, sizeof(g_nor));
	g_nor.suspend_rc = -EBUSY; /* a write / erase holds the driver lock */
	zassert_ok(alp_som_power_bind_flash(DEVICE_DT_GET(EMUL_TMP)));

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL), ALP_ERR_BUSY);
	zassert_equal(level(LPGPIO, FLASH_RST), 1, "the NOR was never reset under the write");
	zassert_equal(g_nor.resumes, 0u, "nothing to give back: the lock was never taken");
	zassert_equal(alp_som_power_quiesced(), 0u, "everything quiesced before it is rolled back");
	zassert_equal(level(LPGPIO, NRST_PIN), 1);

	/* A part that never reports ready is an I/O error, same outcome. */
	g_nor.suspend_rc = -ETIMEDOUT;
	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_RUN, NULL), ALP_ERR_IO);
	zassert_equal(level(LPGPIO, FLASH_RST), 1);
}

/* ---- #2784 U7: RV-3028 wake services, wake decode hooks, STANDBY records ------- */

#define RTC_STATUS_REG   0x0Eu
#define RTC_CONTROL2_REG 0x10u
#define RTC_SF_TF        0x08u
#define RTC_SF_AF        0x04u
#define RTC_SF_UF        0x10u
#define RTC_SF_PORF      0x01u
#define RTC_C2_TIE       0x10u
#define RTC_C2_AIE       0x08u

ZTEST(power_som_domains, test_rtc_wake_service_reports_enabled_flags_and_clears)
{
	uint8_t flags = 0xFFu;

	/* Countdown expired with TIE on: reported; every latched flag is acknowledged
	 * with the constant mask (0 for TF, 1 for every other latchable flag). */
	rtc_regs()[RTC_STATUS_REG]   = RTC_SF_TF | RTC_SF_PORF;
	rtc_regs()[RTC_CONTROL2_REG] = RTC_C2_TIE;
	zassert_ok(alp_som_power_rtc_wake_service(&flags));
	zassert_equal(flags, RTC_SF_TF);
	zassert_equal(rtc_regs()[RTC_STATUS_REG], 0x7Fu & ~RTC_SF_TF);

	/* A latched flag with its interrupt enable off is no wake cause -- but it is
	 * still cleared, or it would sit there forever. */
	rtc_regs()[RTC_STATUS_REG]   = RTC_SF_AF | RTC_SF_UF;
	rtc_regs()[RTC_CONTROL2_REG] = RTC_C2_TIE;
	zassert_ok(alp_som_power_rtc_wake_service(&flags));
	zassert_equal(flags, 0u);
	zassert_equal(rtc_regs()[RTC_STATUS_REG], 0x7Fu & ~(RTC_SF_AF | RTC_SF_UF));

	/* Alarm. */
	rtc_regs()[RTC_STATUS_REG]   = RTC_SF_AF;
	rtc_regs()[RTC_CONTROL2_REG] = RTC_C2_AIE;
	zassert_ok(alp_som_power_rtc_wake_service(&flags));
	zassert_equal(flags, RTC_SF_AF);

	/* Nothing latched: no STATUS write at all. */
	rtc_regs()[RTC_STATUS_REG] = RTC_SF_PORF;
	zassert_ok(alp_som_power_rtc_wake_service(&flags));
	zassert_equal(flags, 0u);
	zassert_equal(rtc_regs()[RTC_STATUS_REG], RTC_SF_PORF, "PORF untouched");

	zassert_equal(alp_som_power_rtc_wake_service(NULL), ALP_ERR_INVAL);
}

ZTEST(power_som_domains, test_rtc_int_armed_reads_control2)
{
	bool armed = true;

	rtc_regs()[RTC_CONTROL2_REG] = 0u;
	zassert_ok(alp_som_power_rtc_int_armed(&armed));
	zassert_false(armed);
	rtc_regs()[RTC_CONTROL2_REG] = RTC_C2_AIE;
	zassert_ok(alp_som_power_rtc_int_armed(&armed));
	zassert_true(armed);
	rtc_regs()[RTC_CONTROL2_REG] = RTC_C2_TIE;
	zassert_ok(alp_som_power_rtc_int_armed(&armed));
	zassert_true(armed);
	rtc_regs()[RTC_CONTROL2_REG] = 0x20u; /* UIE alone is not a wake source */
	zassert_ok(alp_som_power_rtc_int_armed(&armed));
	zassert_false(armed);
	zassert_equal(alp_som_power_rtc_int_armed(NULL), ALP_ERR_INVAL);
}

static void set_rtc_time(uint8_t sec, uint8_t min, uint8_t hr, uint8_t day, uint8_t mon, uint8_t yr)
{
	rtc_regs()[0] = sec;
	rtc_regs()[1] = min;
	rtc_regs()[2] = hr;
	rtc_regs()[4] = day;
	rtc_regs()[5] = mon;
	rtc_regs()[6] = yr;
}

ZTEST(power_som_domains, test_rtc_seconds_since_2000)
{
	uint32_t s = 1u;

	set_rtc_time(0x00, 0x00, 0x00, 0x01, 0x01, 0x00); /* 2000-01-01 00:00:00 */
	zassert_ok(alp_som_power_rtc_seconds(&s));
	zassert_equal(s, 0u);

	set_rtc_time(0x10, 0x00, 0x00, 0x01, 0x01, 0x00);
	zassert_ok(alp_som_power_rtc_seconds(&s));
	zassert_equal(s, 10u);

	set_rtc_time(0x59, 0x59, 0x23, 0x31, 0x12, 0x00); /* 2000-12-31, leap year: day 365 */
	zassert_ok(alp_som_power_rtc_seconds(&s));
	zassert_equal(s, 365u * 86400u + 23u * 3600u + 59u * 60u + 59u);

	set_rtc_time(0x00, 0x00, 0x00, 0x01, 0x03, 0x24); /* 2024-03-01: 8826 days */
	zassert_ok(alp_som_power_rtc_seconds(&s));
	zassert_equal(s, 8826u * 86400u);

	set_rtc_time(0x00, 0x00, 0x00, 0x01, 0x03, 0x23); /* 2023-03-01: 8826 - 366 days */
	zassert_ok(alp_som_power_rtc_seconds(&s));
	zassert_equal(s, (8826u - 366u) * 86400u);

	/* A one-second step is exactly one second (the slept-time arithmetic). */
	uint32_t a, b;

	set_rtc_time(0x59, 0x59, 0x23, 0x28, 0x02, 0x24);
	zassert_ok(alp_som_power_rtc_seconds(&a));
	set_rtc_time(0x00, 0x00, 0x00, 0x29, 0x02, 0x24); /* 2024-02-29 exists */
	zassert_ok(alp_som_power_rtc_seconds(&b));
	zassert_equal(b - a, 1u);
}

ZTEST(power_som_domains, test_rtc_seconds_rejects_garbage)
{
	uint32_t s = 5u;

	set_rtc_time(0x7A, 0x00, 0x00, 0x01, 0x01, 0x00); /* not BCD */
	zassert_equal(alp_som_power_rtc_seconds(&s), ALP_ERR_IO);
	set_rtc_time(0x00, 0x00, 0x00, 0x00, 0x01, 0x00); /* day 0 */
	zassert_equal(alp_som_power_rtc_seconds(&s), ALP_ERR_IO);
	set_rtc_time(0x00, 0x00, 0x00, 0x01, 0x13, 0x00); /* month 13 */
	zassert_equal(alp_som_power_rtc_seconds(&s), ALP_ERR_IO);
	set_rtc_time(0x60, 0x00, 0x00, 0x01, 0x01, 0x00); /* second 60 */
	zassert_equal(alp_som_power_rtc_seconds(&s), ALP_ERR_IO);
	zassert_equal(alp_som_power_rtc_seconds(NULL), ALP_ERR_INVAL);
}

ZTEST(power_som_domains, test_wake_gpio_is_the_rtc_int_pad)
{
	const struct gpio_dt_spec *g = alp_som_power_wake_gpio(ALP_POWER_DOMAIN_RTC);

	zassert_not_null(g);
	zassert_equal(g->pin, 0);
	zassert_equal(g->port, LPGPIO);
	zassert_equal(g->dt_flags & GPIO_ACTIVE_LOW, GPIO_ACTIVE_LOW, "/INT is active low");
	zassert_is_null(alp_som_power_wake_gpio(ALP_POWER_DOMAIN_WIFI_BLE), "no wake input");
	zassert_is_null(alp_som_power_wake_gpio(ALP_POWER_DOMAIN_COUNT));
}

/* The strong definitions below replace the weak no-ops in som_power.c for this
 * image: the default (no backend) behaviour is "the record's fields pass through
 * untouched", which test_weak_decode_defaults_pass_the_record_through proves by
 * leaving the hooks disarmed. */
static struct {
	bool                armed, discard;
	unsigned            early_calls, i2c_calls;
	unsigned            early_order, i2c_order, seq;
	alp_som_pd_record_t early_seen;
} g_dec;

void alp_som_power_wake_decode_early(alp_som_pd_record_t *rec)
{
	if (!g_dec.armed) {
		return;
	}
	g_dec.early_calls++;
	g_dec.early_order = ++g_dec.seq;
	g_dec.early_seen  = *rec;
	rec->wake_source |= ALP_POWER_WAKE_TIMER;
}

bool alp_som_power_wake_decode_i2c(alp_som_pd_record_t *rec)
{
	if (!g_dec.armed) {
		return true;
	}
	g_dec.i2c_calls++;
	g_dec.i2c_order = ++g_dec.seq;
	if (g_dec.discard) {
		return false;
	}
	rec->wake_source |= ALP_POWER_WAKE_RTC;
	rec->slept_ms = 4242u;
	return true;
}

static void poke_cycle_record(alp_power_mode_t mode)
{
	alp_som_pd_record_t r = {
		.mode        = (uint32_t)mode,
		.quiesced    = ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_WIFI_BLE),
		.armed       = ALP_POWER_WAKE_TIMER,
		.armed_hw    = ALP_SOM_ARM_LPTIMER,
		.armed_ms    = 500u,
		.entry_rtc_s = 1234u,
	};

	alp_som_pd_store_save(&r);
}

/* The same record, but the way the STOP backend writes it when the NSRST syndrome bit was
 * probed before the sleep and does clear. */
static void poke_trusted_record(alp_power_mode_t mode)
{
	alp_som_pd_record_t r;

	poke_cycle_record(mode);
	zassert_true(alp_som_pd_store_load(&r));
	r.armed_hw |= ALP_SOM_REC_NSRST_TRUSTED;
	alp_som_pd_store_save(&r);
}

ZTEST(power_som_domains, test_weak_decode_defaults_pass_the_record_through)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec)); /* hooks disarmed: they return at once */
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(info.wake_source, 0u, "nothing decoded: nothing claimed");
	zassert_equal(info.slept_ms, 0u);
}

ZTEST(power_som_domains, test_boot_runs_the_decode_hooks_early_then_i2c)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed = true;
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;

	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(g_dec.early_calls, 1u);
	zassert_equal(g_dec.i2c_calls, 0u, "the I2C half waits for the controller");
	/* the hook sees the whole cycle record, armed fields included */
	zassert_equal(g_dec.early_seen.armed_hw, ALP_SOM_ARM_LPTIMER);
	zassert_equal(g_dec.early_seen.armed_ms, 500u);
	zassert_equal(g_dec.early_seen.entry_rtc_s, 1234u);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER, "part 1 is already visible");

	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_equal(g_dec.i2c_calls, 1u);
	zassert_true(g_dec.early_order < g_dec.i2c_order);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER | ALP_POWER_WAKE_RTC);
	zassert_equal(info.slept_ms, 4242u);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);

	g_dec.armed = false;
}

ZTEST(power_som_domains, test_decode_hooks_are_not_run_without_a_valid_record)
{
	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed = true;
	g_stop_mode = 0u;
	alp_som_pd_store_clear();
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_equal(g_dec.early_calls, 0u, "a plain POR decodes nothing");
	zassert_equal(g_dec.i2c_calls, 0u);

	/* STOP record, STOP_MODE_STAT = 0: dropped before any decode. */
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(g_dec.early_calls, 0u);
	g_dec.armed = false;
}

ZTEST(power_som_domains, test_standby_record_is_restored_without_stop_mode_stat)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STANDBY, NULL));
	zassert_equal(level(LPGPIO, NRST_PIN), 0);
	alp_som_pd_record_t rec;
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0u; /* the register names STOP only */

	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STANDBY);
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "the held domain was put back, not stranded");
}

ZTEST(power_som_domains, test_record_layout_is_stable)
{
	zassert_equal(sizeof(alp_som_pd_record_t), 60u);
	zassert_equal(offsetof(alp_som_pd_record_t, crc), 56u);
}

ZTEST(power_som_domains, test_discarded_decode_reports_a_plain_boot)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed   = true;
	g_dec.discard = true;
	poke_cycle_record(ALP_POWER_MODE_STANDBY);
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid, "an untrusted STANDBY record is not reported");
	zassert_equal(info.wake_source, 0u);
	g_dec.armed = false;
}

/* ---- A STOP wake with no usable record ------------------------------------------- */

static void hold_everything(void)
{
	zassert_ok(gpio_pin_configure(LPGPIO, NRST_PIN, GPIO_OUTPUT_LOW));
	zassert_ok(gpio_pin_configure(LPGPIO, PHY_PWR, GPIO_OUTPUT_LOW));
	zassert_ok(gpio_pin_configure(LPGPIO, FLASH_RST, GPIO_OUTPUT_LOW));
}

ZTEST(power_som_domains, test_stop_wake_without_a_record_releases_every_pad)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	alp_som_pd_store_clear();
	hold_everything();
	g_stop_mode = 0x10u; /* STOP wake, but the SRAM did not keep the record */

	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "CC3501E nRESET released");
	zassert_equal(level(LPGPIO, PHY_PWR), 1, "PHY powered");
	zassert_equal(level(LPGPIO, FLASH_RST), 1, "NOR out of reset");
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid, "reported, not silent");
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(info.wake_source, 0u, "the cause is unknown");
	zassert_not_equal(info.quiesced_domains, 0u);
	zassert_equal(info.restore_failed_domains, 0u);
	zassert_equal(info.quiesced_domains, info.restored_domains);
}

ZTEST(power_som_domains, test_corrupt_record_on_a_stop_wake_is_a_blind_restore)
{
	alp_power_boot_info_t info;
	alp_som_pd_record_t   r = { .magic = ALP_SOM_PD_RECORD_MAGIC, .mode = 4u, .crc = 1u };

	memset(&g_dec, 0, sizeof(g_dec));
	hold_everything();
	alp_som_pd_store_poke(&r); /* right magic, wrong CRC */
	g_stop_mode = 0x10u;
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
}

ZTEST(power_som_domains, test_plain_por_without_a_record_still_touches_nothing)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	alp_som_pd_store_clear();
	hold_everything();
	g_stop_mode = 0u; /* not a STOP wake */
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_equal(level(LPGPIO, NRST_PIN), 0, "left exactly as the reset left it");
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_false(info.valid);
}

/* ---- A pin reset is not a wake -------------------------------------------------- */

static uint32_t g_syndrome; /* what AON.RTSS_HE_RESET reads this boot */

uint32_t alp_som_power_reset_syndrome_take(void)
{
	uint32_t v = g_syndrome;

	g_syndrome = 0u; /* acknowledged: read-to-clear */
	return v;
}

static void wake_boot(alp_power_boot_info_t *info)
{
	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_equal(alp_som_power_boot_restore_i2c(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(info));
}

ZTEST(power_som_domains, test_se_initiated_reset_after_stop_is_a_wake)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed = true;
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	g_syndrome  = 0u; /* POR / Secure-Enclave-initiated */
	wake_boot(&info);
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER | ALP_POWER_WAKE_RTC);
	g_dec.armed = false;
}

ZTEST(power_som_domains, test_pin_reset_after_stop_is_an_aborted_sleep_not_a_wake)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed = true;
	hold_everything();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP, NULL));
	alp_som_pd_record_t rec;
	zassert_true(alp_som_pd_store_load(&rec));
	rec.armed_hw |= ALP_SOM_REC_NSRST_TRUSTED; /* probed before the sleep: the bit does clear */
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);
	g_stop_mode = 0x10u;
	g_syndrome  = 1u; /* NSRST asserted: a debugger nRESET */
	wake_boot(&info);

	zassert_true(info.valid, "the cycle is reported");
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN, "the sleep was not realised");
	zassert_equal(info.wake_source, 0u, "no wake cause, even if the decode found a flag");
	zassert_equal(info.slept_ms, 0u);
	zassert_not_equal(info.quiesced_domains, 0u);
	zassert_equal(info.quiesced_domains, info.restored_domains, "the domains are still put back");
	zassert_equal(level(LPGPIO, NRST_PIN), 1);
	zassert_equal(g_syndrome, 0u, "the syndrome was acknowledged");
	g_dec.armed = false;
}

ZTEST(power_som_domains, test_a_stale_nsrst_bit_never_turns_a_wake_into_an_aborted_sleep)
{
	alp_power_boot_info_t info;

	/* The syndrome bit could not be shown to clear before the sleep (no TRUSTED flag, e.g.
	 * a write-only field that reads 1 forever): a set bit at the next boot is not a pin reset. */
	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed = true;
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	g_syndrome  = 1u;
	wake_boot(&info);
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP, "still a wake");
	zassert_equal(info.wake_source, ALP_POWER_WAKE_TIMER | ALP_POWER_WAKE_RTC);
	g_dec.armed = false;
}

ZTEST(power_som_domains, test_pin_reset_with_no_record_is_not_classified_the_bit_is_unproven)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	alp_som_pd_store_clear();
	hold_everything();
	g_stop_mode = 0x10u;
	g_syndrome  = 1u;
	wake_boot(&info);
	zassert_true(info.valid);
	/* With no record there is no proof the bit clears, so it is not read as a pin reset. */
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(info.wake_source, 0u);
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "still released");
}

ZTEST(power_som_domains, test_only_the_pin_bit_marks_an_external_reset)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	poke_trusted_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	g_syndrome  = 4u; /* reset request to the power domain: not proven to be a pin reset */
	wake_boot(&info);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
}

ZTEST(power_som_domains, test_run_cycle_record_ignores_the_syndrome)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	poke_cycle_record(ALP_POWER_MODE_RUN);
	g_stop_mode = 0u;
	g_syndrome  = 1u;
	wake_boot(&info);
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_RUN);
}

/* ---- Image identity and the sticky STOP_MODE_STAT (bench U8d) -------------------------- */

bool alp_som_power_stop_mode_stat_clear(void)
{
	g_stat_clears++;
	g_stat_clear_order = ++g_dec.seq;
	g_stop_mode &= ~0x10u; /* W1C of bit 4 only */
	return true;
}

ZTEST(power_som_domains, test_stop_mode_stat_is_acknowledged_after_the_decode)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_dec.armed   = true;
	g_stat_clears = 0u;
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	wake_boot(&info);
	zassert_equal(g_stat_clears, 1u);
	zassert_true(g_stat_clear_order > g_dec.i2c_order, "after both decode parts");
	zassert_equal(g_stop_mode & 0x10u, 0u, "the sticky status is gone");
	zassert_equal(g_stop_mode & 0x1u, 0u, "and STOP_MODE_CTRL (bit 0) was never touched");
	g_dec.armed = false;
	(void)g_i2c_decode_order_seen;
}

ZTEST(power_som_domains, test_a_second_reset_after_a_wake_is_not_a_wake)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	hold_everything();
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	wake_boot(&info);
	zassert_true(info.valid);

	/* A flash / debugger reset after that: the status was acknowledged, there is no record. */
	hold_everything();
	wake_boot(&info);
	zassert_false(info.valid, "not a STOP wake");
	zassert_equal(level(LPGPIO, NRST_PIN), 0, "and nothing was blindly released");
}

ZTEST(power_som_domains, test_another_images_bench_cell_starts_a_fresh_run)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_image = 0xA1111111u;
	alp_som_pd_store_clear();
	alp_som_pd_bench_set(3u);
	zassert_equal(alp_som_pd_bench_count(), 3u);

	/* A clean flash of a different image on top, with the old run's STOP_MODE_STAT still set. */
	g_image = 0xB2222222u;
	zassert_equal(alp_som_pd_bench_count(), 0u, "another image's counter reads 0");
	zassert_true(alp_som_pd_bkram_foreign());
	hold_everything();
	g_stop_mode = 0x10u;
	wake_boot(&info);
	zassert_false(info.valid, "the flash's reset is not reported as a STOP wake");
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "but what the old run left held is released");
	zassert_false(alp_som_pd_bkram_foreign(), "adopted");
	zassert_equal(alp_som_pd_bench_count(), 0u);
	g_image = 0xA1111111u;
}

ZTEST(power_som_domains, test_a_record_from_another_image_is_dropped)
{
	alp_power_boot_info_t info;
	alp_som_pd_record_t   rec;

	memset(&g_dec, 0, sizeof(g_dec));
	g_image = 0xA1111111u;
	poke_cycle_record(ALP_POWER_MODE_STOP); /* stamped with image A */
	zassert_true(alp_som_pd_store_load(&rec));
	zassert_equal(rec.image_id, 0xA1111111u);

	g_image = 0xB2222222u;
	hold_everything();
	g_stop_mode = 0x10u;
	wake_boot(&info);
	zassert_false(info.valid, "not reported as a wake");
	zassert_false(alp_som_pd_store_load(&rec), "the record is gone");
	zassert_equal(level(LPGPIO, NRST_PIN), 1, "the domains the record names are restored anyway");
	g_image = 0xA1111111u;
}

ZTEST(power_som_domains, test_the_same_image_still_wakes_normally)
{
	alp_power_boot_info_t info;

	memset(&g_dec, 0, sizeof(g_dec));
	g_image = 0xA1111111u;
	alp_som_pd_bench_set(1u);
	poke_cycle_record(ALP_POWER_MODE_STOP);
	g_stop_mode = 0x10u;
	wake_boot(&info);
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(alp_som_pd_bench_count(), 1u, "the counter survives a wake of the same image");
}
