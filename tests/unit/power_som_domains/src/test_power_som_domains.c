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

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <alp/chips/tmp112.h>
#include <alp/power.h>

#include "fakes.h"
#include "som_power.h"
#include "som_power_chips.h"

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
static alp_power_domain_t g_fail_on = ALP_POWER_DOMAIN_COUNT;

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
	g_log[g_log_n]           = (alp_power_domain_t)(uintptr_t)ctx;
	g_log_restore[g_log_n++] = true;
	return ALP_OK;
}

static const alp_som_power_hooks_t log_hooks = { .quiesce = log_quiesce, .restore = log_restore };

static void bind_all_logging(void)
{
	g_log_n   = 0;
	g_fail_on = ALP_POWER_DOMAIN_COUNT;
	for (int d = 0; d < (int)ALP_POWER_DOMAIN_COUNT; ++d) {
		alp_power_domain_info_t i;
		(void)alp_som_power_ops_domain_info((alp_power_domain_t)d, &i);
		if (i.present) {
			zassert_ok(alp_som_power_bind((alp_power_domain_t)d, &log_hooks, (void *)(uintptr_t)d));
		}
	}
}

ZTEST(power_som_domains, test_quiesce_consumers_first_restore_in_reverse)
{
	static const alp_power_domain_t want[] = {
		ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_EXT_FLASH, ALP_POWER_DOMAIN_TEMP_SENSOR,
		ALP_POWER_DOMAIN_RTC,      ALP_POWER_DOMAIN_BACKLIGHT, ALP_POWER_DOMAIN_ETH_PHY,
	};
	uint32_t failed = 99u;

	bind_all_logging();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
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

	zassert_equal(alp_som_power_quiesce(ALP_POWER_MODE_STOP), ALP_ERR_IO);
	/* wifi, flash, temp quiesced, rtc failed -> the three come back, newest first. */
	zassert_equal(g_log_n, 7u);
	zassert_equal(g_log[4], ALP_POWER_DOMAIN_TEMP_SENSOR);
	zassert_equal(g_log[5], ALP_POWER_DOMAIN_EXT_FLASH);
	zassert_equal(g_log[6], ALP_POWER_DOMAIN_WIFI_BLE);
	zassert_equal(alp_som_power_quiesced(), 0u);
	zassert_equal(alp_som_power_restore(NULL), ALP_ERR_NOT_READY, "no record was written");
}

ZTEST(power_som_domains, test_keep_alive_policy_is_never_touched)
{
	bind_all_logging();
	zassert_ok(alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_BACKLIGHT, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
	for (size_t i = 0; i < g_log_n; ++i) {
		zassert_not_equal(g_log[i], ALP_POWER_DOMAIN_BACKLIGHT);
	}
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_BACKLIGHT), ALP_SOM_PD_ACTIVE);
	zassert_ok(alp_som_power_restore(NULL));
}

ZTEST(power_som_domains, test_sleep_modes_do_not_quiesce)
{
	bind_all_logging();
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_SLEEP));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_DEEP_SLEEP));
	zassert_equal(g_log_n, 0u);
	zassert_equal(alp_som_power_quiesced(), 0u);
	zassert_equal(alp_som_power_quiesce((alp_power_mode_t)99), ALP_ERR_INVAL);

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STANDBY));
	zassert_equal(g_log_n, 6u, "STANDBY quiesces like STOP");
	zassert_ok(alp_som_power_restore(NULL));
}

/* ---- Default pin / I2C actions -------------------------------------------------- */

ZTEST(power_som_domains, test_default_actions_and_restore_levels)
{
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));

	zassert_equal(level(LPGPIO, NRST_PIN), 0, "CC3501E nRESET held low");
	zassert_equal(level(LPGPIO, WIFIEN), 1, "AUTO never touches WIFI_EN");
	zassert_equal(level(LPGPIO, PHY_PWR), 0, "E_PHY_PWRDWN low (also gates the Y3 oscillator)");
	zassert_equal(level(LPGPIO, FLASH_RST), 0, "OSPI1_RESETn held low");
	zassert_equal(level(GPIO5, BL_PIN), 0, "backlight EN low");
	zassert_true((tmp_conf() & 0x0100u) != 0u, "TMP112 CONF.SD set");
	zassert_equal(tmp_conf() & ~0x0100u, 0x60A0u, "other CONF bits kept");
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x87u, "CLKOUT_FD = low, CLKOE untouched");
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
	zassert_equal(alp_som_power_state(ALP_POWER_DOMAIN_ETH_PHY), ALP_SOM_PD_ACTIVE);
}

ZTEST(power_som_domains, test_backlight_that_was_off_stays_off)
{
	zassert_ok(gpio_pin_configure(GPIO5, BL_PIN, GPIO_OUTPUT_LOW | GPIO_INPUT));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));
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

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
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
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
	zassert_ok(alp_som_power_restore(NULL));
	zassert_false(g_fw.initialised, "restore does not initialise what was never up");
}

#ifdef ALP_TEST_RAIL_OFF
ZTEST(power_som_domains, test_cc3501e_rail_off_uses_power_off_then_hard_reset)
{
	zassert_ok(alp_som_power_bind_cc3501e(&g_fw));
	zassert_ok(alp_som_power_ops_policy_set(
	    NULL, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_RAIL_OFF));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
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
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));
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
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_STOP));
	uint32_t held = alp_som_power_quiesced();
	zassert_not_equal(held, 0u);
	zassert_equal(level(LPGPIO, NRST_PIN), 0);

	alp_som_pd_record_t rec;
	zassert_true(alp_som_pd_store_load(&rec));
	alp_som_power_reset_for_test();
	alp_som_pd_store_save(&rec);

	zassert_equal(alp_som_power_boot_restore(), 0);
	zassert_ok(alp_som_power_ops_boot_wake_info(&info));
	zassert_true(info.valid);
	zassert_equal(info.realised_mode, ALP_POWER_MODE_STOP);
	zassert_equal(info.quiesced_domains, held);
	zassert_equal(info.restored_domains, held);
	zassert_equal(info.restore_failed_domains, 0u);
	assert_pins_untouched();
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x87u, "restore leaves the RTC CLKOUT off");

	/* The record is consumed: a second boot pass is a plain POR. */
	alp_som_pd_record_t gone;
	zassert_false(alp_som_pd_store_load(&gone));
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
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));
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

	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));
	zassert_equal(alp_som_power_bind(ALP_POWER_DOMAIN_RTC, &log_hooks, NULL),
	              ALP_ERR_BUSY,
	              "no re-binding while a domain is held");
	zassert_equal(alp_som_power_ops_policy_set(
	                  NULL, ALP_POWER_DOMAIN_RTC, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE),
	              ALP_ERR_BUSY);
	zassert_ok(alp_som_power_restore(NULL));
}

ZTEST(power_som_domains, test_rv3028_hook_routes_clkout_low_and_nothing_else)
{
	rv3028c7_t ctx = { 0 };

	zassert_ok(alp_som_power_bind_rv3028(&ctx));
	zassert_ok(alp_som_power_quiesce(ALP_POWER_MODE_RUN));
	zassert_equal(g_clkout_calls, 1u);
	zassert_equal(g_clkout_src, (int)RV3028C7_CLKOUT_LOW);
	zassert_equal(rtc_regs()[RTC_CLKOUT_REG], 0x80u, "the DT I2C path was bypassed");
	zassert_ok(alp_som_power_restore(NULL));
	zassert_equal(g_clkout_calls, 1u, "restore leaves the RTC alone");
}
