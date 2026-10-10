/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * SoM power-domain runtime (#2784, unit U5): registry, policy, quiesce /
 * restore and the cold-boot restore.  See som_power.h for the model.
 *
 * Registry
 * --------
 * One const entry per domain, built at compile time from the generated
 * `alp,som-power-domain` devicetree nodes.  A SKU that does not fit a part has
 * no node, so its entry stays zero and the domain reads as not present.  The
 * pins come from the node's reset / enable / powerdown GPIO specs, whose flag
 * is the ASSERTED level: gpio_pin_set(spec, 1) means "drive the pad to the
 * level that holds reset / powers down / enables", whatever that pad level is.
 *
 * Default actions (what AUTO does)
 * --------------------------------
 *   wifi_ble     hold E_WIFI_NRST (P15_1) low.  Restore releases nRESET -- the
 *                cc3501e_hard_reset() semantics, rails stay up.  The
 *                supply-cycling cc3501e_reset() is NEVER used: it drops WIFI_EN
 *                for 50 ms, the path that may never relaunch on an activated
 *                unit.  RAIL_OFF (opt-in, CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF)
 *                additionally gates WIFI_EN, after nRESET is already low.
 *   eth_phy      E_PHY_PWRDWN (P15_4) low.  That net is also the TRI pin of the
 *                Y3 50 MHz PHY reference oscillator, so the clock stops with the
 *                PHY.  Restore: P15_4 high, then an E_PHY_RESET pulse.
 *   ext_flash    OSPI1_RESETn (P15_7) held low.  flash_ospi_alif exposes no
 *                power-management or deep-power-down hook, so reset-hold is the
 *                action; after it the part is back in 1-1-1 SPI, which the
 *                driver is told about (som_power_flash.c).
 *   ext_ram      OSPI0_RESETn (P15_6) held low.  No HyperRAM driver exists in
 *                the tree to issue the part's low-power command.
 *   temp_sensor  TMP112 CONFIG.SD set over BRD_I2C.
 *   rtc          RV-3028 stays powered (KEEP_ALIVE); only CLKOUT is switched
 *                off, in the RAM mirror (no EEPROM write cycle).
 *   backlight    P5_5 low.  Main-domain pad, does not hold through STOP.
 *
 * State is tracked in software: an unpowered PHY answers MDIO with stale data,
 * not 0xFFFF, so the chip can never be asked whether it is quiesced.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"
#include "som_power.h"

#define DT_DRV_COMPAT alp_som_power_domain

#ifndef CONFIG_ALP_SDK_SOM_PD_WIFI_SETTLE_MS
#define CONFIG_ALP_SDK_SOM_PD_WIFI_SETTLE_MS 3500
#endif

/* Datasheet / bench timing.  Early (cold-boot) waits are busy-waits and kept
 * short; the drivers that initialise later wait for their own chip. */
#define SOMPD_NRST_MIN_HOLD_MS 50u  /* cc3501e_hard_reset(): 50 ms nRESET pulse */
#define SOMPD_RAIL_UP_MS       20u  /* WIFI_EN high -> VPA settled              */
#define SOMPD_PHY_PWRUP_MS     20u  /* P15_4 high -> oscillator + PHY powered   */
#define SOMPD_PHY_RESET_MS     10u  /* E_PHY_RESET pulse width                  */
#define SOMPD_PHY_SETTLE_MS    200u /* PHY ready for MDIO after reset (runtime) */
#define SOMPD_PHY_SETTLE_EARLY 20u
#define SOMPD_MEM_RESET_MS     10u /* NOR / HyperRAM recovery after RESETn high */

/* ---- Registry ------------------------------------------------------------- */

typedef enum {
	SOMPD_A_HOLD_RESET = 0,
	SOMPD_A_POWERDOWN_PIN,
	SOMPD_A_DEEP_POWER_DOWN_CMD,
	SOMPD_A_SHUTDOWN_REG,
	SOMPD_A_KEEP_ALIVE,
	SOMPD_A_ENABLE_LOW,
} sompd_action_t;

typedef struct {
	bool                present;
	bool                holds_through_stop;
	bool                rail_off_opt_in;
	uint8_t             action; /* sompd_action_t of AUTO */
	uint8_t             modes;  /* BIT(alp_power_mode_t) the default action applies to */
	uint8_t             dependents;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec enable;
	struct gpio_dt_spec powerdown;
	struct gpio_dt_spec wake; /* wake-gpios: an INPUT that wakes the SoC (RV-3028 /INT) */
	struct i2c_dt_spec  i2c;  /* alp,device when it sits on an I2C bus */
} sompd_t;

#define SOMPD_ROLE_wifi_ble    ALP_POWER_DOMAIN_WIFI_BLE
#define SOMPD_ROLE_eth_phy     ALP_POWER_DOMAIN_ETH_PHY
#define SOMPD_ROLE_ext_flash   ALP_POWER_DOMAIN_EXT_FLASH
#define SOMPD_ROLE_ext_ram     ALP_POWER_DOMAIN_EXT_RAM
#define SOMPD_ROLE_temp_sensor ALP_POWER_DOMAIN_TEMP_SENSOR
#define SOMPD_ROLE_rtc         ALP_POWER_DOMAIN_RTC
#define SOMPD_ROLE_backlight   ALP_POWER_DOMAIN_BACKLIGHT

#define SOMPD_ACT_hold_reset          SOMPD_A_HOLD_RESET
#define SOMPD_ACT_powerdown_pin       SOMPD_A_POWERDOWN_PIN
#define SOMPD_ACT_deep_power_down_cmd SOMPD_A_DEEP_POWER_DOWN_CMD
#define SOMPD_ACT_shutdown_reg        SOMPD_A_SHUTDOWN_REG
#define SOMPD_ACT_keep_alive          SOMPD_A_KEEP_ALIVE
#define SOMPD_ACT_enable_low          SOMPD_A_ENABLE_LOW

#define SOMPD_MODE_stop       BIT(ALP_POWER_MODE_STOP)
#define SOMPD_MODE_standby    BIT(ALP_POWER_MODE_STANDBY)
#define SOMPD_MODE_sleep      BIT(ALP_POWER_MODE_SLEEP)
#define SOMPD_MODE_deep_sleep BIT(ALP_POWER_MODE_DEEP_SLEEP)

#define SOMPD_DEP_cam_ldo    ALP_POWER_DEP_CAM_LDO
#define SOMPD_DEP_sd_enable  ALP_POWER_DEP_SD_EN
#define SOMPD_DEP_phy_refclk ALP_POWER_DEP_PHY_REFCLK

#define SOMPD_MODE_BIT(n, p, i) UTIL_CAT(SOMPD_MODE_, DT_STRING_TOKEN_BY_IDX(n, p, i))
#define SOMPD_DEP_BIT(n, p, i)  UTIL_CAT(SOMPD_DEP_, DT_STRING_TOKEN_BY_IDX(n, p, i))

/* DEVICE_DT_GET_OR_NULL: a pad whose GPIO controller is disabled in this build
 * gets a NULL port (reads as "pin unusable") instead of a link error; the I2C
 * bus is likewise NULL when CONFIG_I2C is off (no driver, no device object). */
#define SOMPD_GPIO(n, prop) \
	COND_CODE_1(DT_NODE_HAS_PROP(n, prop), \
	            ({ \
	                .port     = DEVICE_DT_GET_OR_NULL(DT_GPIO_CTLR_BY_IDX(n, prop, 0)), \
	                .pin      = DT_GPIO_PIN_BY_IDX(n, prop, 0), \
	                .dt_flags = DT_GPIO_FLAGS_BY_IDX(n, prop, 0), \
	            }), \
	            ({ 0 }))

#define SOMPD_I2C(n) \
	COND_CODE_1( \
	    DT_NODE_HAS_PROP(n, alp_device), \
	    ({ \
	        .bus = COND_CODE_1( \
	            CONFIG_I2C, (DEVICE_DT_GET_OR_NULL(DT_BUS(DT_PROP(n, alp_device)))), (NULL)), \
	        .addr = DT_REG_ADDR(DT_PROP(n, alp_device)), \
	    }), \
	    ({ 0 }))

#define SOMPD_ENTRY(n) \
	[UTIL_CAT(SOMPD_ROLE_, DT_STRING_TOKEN(n, alp_role))] = { \
		.present            = true, \
		.holds_through_stop = DT_ENUM_HAS_VALUE(n, alp_stop_hold, yes), \
		.rail_off_opt_in    = DT_PROP(n, alp_rail_off_opt_in), \
		.action             = UTIL_CAT(SOMPD_ACT_, DT_STRING_TOKEN(n, alp_default_action)), \
		.modes              = (DT_FOREACH_PROP_ELEM_SEP(n, alp_default_modes, SOMPD_MODE_BIT, (|))), \
		.dependents = \
		    COND_CODE_1(DT_NODE_HAS_PROP(n, alp_dependents), \
		                ((DT_FOREACH_PROP_ELEM_SEP(n, alp_dependents, SOMPD_DEP_BIT, (|)))), \
		                (0)), \
		.reset     = SOMPD_GPIO(n, reset_gpios), \
		.enable    = SOMPD_GPIO(n, enable_gpios), \
		.powerdown = SOMPD_GPIO(n, powerdown_gpios), \
		.wake      = SOMPD_GPIO(n, wake_gpios), \
		.i2c       = SOMPD_I2C(n), \
	},

static const sompd_t _reg[ALP_POWER_DOMAIN_COUNT] = { DT_FOREACH_STATUS_OKAY(alp_som_power_domain,
	                                                                         SOMPD_ENTRY) };

/* ---- Mutable state -------------------------------------------------------- */

typedef struct {
	bool     valid;
	uint32_t mode;
	uint32_t wake_source;
	uint32_t slept_ms;
	uint32_t quiesced;
	uint32_t restored;
	uint32_t failed;
} boot_capture_t;

static alp_power_domain_policy_t    _policy[ALP_POWER_DOMAIN_COUNT];
static alp_som_pd_state_t           _state[ALP_POWER_DOMAIN_COUNT];
static const alp_som_power_hooks_t *_hooks[ALP_POWER_DOMAIN_COUNT];
static void                        *_hook_ctx[ALP_POWER_DOMAIN_COUNT];
static int64_t                      _assert_ms[ALP_POWER_DOMAIN_COUNT];
static boot_capture_t               _boot;
static alp_som_pd_record_t          _boot_rec; /* the cycle's record, kept for the wake decode */
static bool                         _boot_external; /* this boot followed a pin / external reset */
static bool _ignore_stat; /* BKRAM held another image's data: STOP_MODE_STAT means nothing here */
/* The record of the quiesce in progress, kept in plain RAM as well: a same-boot rollback
 * (a sleep that did not power down, a refusal after the quiesce) must not depend on BKRAM,
 * whose contents (or whose very retention) the sleep sequence may have disturbed.  The
 * cold-boot wake still reads the BKRAM record; this copy dies with the boot. */
static alp_som_pd_record_t _ram_rec;
static bool                _ram_rec_valid;
static bool _boot_suppressed; /* ... and nothing about this boot is reported as a wake */

K_MUTEX_DEFINE(_lock);

/* Consumers first; restore walks this backwards.  The PHY is last in, first out. */
static const alp_power_domain_t _order[ALP_POWER_DOMAIN_COUNT] = {
	ALP_POWER_DOMAIN_WIFI_BLE,    ALP_POWER_DOMAIN_EXT_FLASH, ALP_POWER_DOMAIN_EXT_RAM,
	ALP_POWER_DOMAIN_TEMP_SENSOR, ALP_POWER_DOMAIN_RTC,       ALP_POWER_DOMAIN_BACKLIGHT,
	ALP_POWER_DOMAIN_ETH_PHY,
};

/* ---- Pin and I2C primitives ------------------------------------------------ */

static void wait_ms(uint32_t ms, bool early)
{
	if (early) {
		k_busy_wait(ms * 1000u);
	} else {
		k_msleep((int32_t)ms);
	}
}

/* Drive @p s to its asserted (true) or released (false) level. */
/* Weak so the host tests can stand in for a pad read-back. */
__weak int alp_som_power_pad_read(const struct gpio_dt_spec *s)
{
	return gpio_pin_get_dt(s);
}

static bool _pads_applied;

static alp_status_t pin_assert(const struct gpio_dt_spec *s, bool asserted)
{
	if (s->port == NULL || !device_is_ready(s->port)) {
		/* A pad whose GPIO controller is disabled in the devicetree: the board or the
		 * app overlay must enable it (&gpio5 / &gpio11 for the backlight and PHY reset). */
		printk("som_power: pad P?_%u unusable: GPIO controller %s\n",
		       (unsigned)s->pin,
		       (s->port == NULL) ? "disabled in the devicetree" : "not ready");
		return ALP_ERR_NOT_READY;
	}
	/* The pads are muxed and pad-configured by the node's pinctrl-0 state, applied
	 * once before the first drive.  A missing or failing state refuses the drive:
	 * levels written to an unmuxed pad would be reported as success and do nothing. */
	if (!_pads_applied) {
		alp_status_t pa = alp_som_power_pads_apply();
		if (pa != ALP_OK) {
			return pa;
		}
		_pads_applied = true;
	}
	int rc = gpio_pin_configure_dt(s, asserted ? GPIO_OUTPUT_ACTIVE : GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		return ALP_ERR_IO;
	}
	/* Read the pad back: the pinctrl group enables the pad's input buffer, so the
	 * port's external data register shows what the pad actually carries.  A
	 * mismatch means the hold did not happen (pad not muxed, shorted, wrong
	 * controller) and the caller must not enter the sleep believing it did.  A
	 * negative read is "cannot verify", not a failure. */
	int seen = alp_som_power_pad_read(s);
	if (seen >= 0 && (seen != 0) != asserted) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

#define TMP112_REG_CONF 0x01u
#define TMP112_CONF_SD  0x0100u

static alp_status_t tmp112_shutdown(const struct i2c_dt_spec *i2c, bool shutdown)
{
	if (i2c->bus == NULL || !device_is_ready(i2c->bus)) {
		return ALP_ERR_NOT_READY;
	}
	uint8_t reg = TMP112_REG_CONF;
	uint8_t buf[3];
	if (i2c_write_read_dt(i2c, &reg, 1, &buf[1], 2) != 0) {
		return ALP_ERR_IO;
	}
	uint16_t conf = (uint16_t)(((uint16_t)buf[1] << 8) | buf[2]);
	conf   = shutdown ? (uint16_t)(conf | TMP112_CONF_SD) : (uint16_t)(conf & ~TMP112_CONF_SD);
	buf[0] = TMP112_REG_CONF;
	buf[1] = (uint8_t)(conf >> 8);
	buf[2] = (uint8_t)conf;
	return (i2c_write_dt(i2c, buf, sizeof(buf)) == 0) ? ALP_OK : ALP_ERR_IO;
}

#define RV3028_REG_CONTROL_1  0x0Fu
#define RV3028_CTRL1_EERD     0x08u /* RAM bit: pause the 24 h EEPROM -> mirror refresh */
#define RV3028_REG_EE_CLKOUT  0x35u
#define RV3028_CLKOUT_FD_MASK 0x07u
#define RV3028_CLKOUT_FD_LOW  0x07u /* CLKOUT driven low (rv3028c7_route_clkout LOW) */

static alp_status_t rv3028_eerd(const struct i2c_dt_spec *i2c, bool pause)
{
	if (i2c->bus == NULL || !device_is_ready(i2c->bus)) {
		return ALP_ERR_NOT_READY;
	}
	return (i2c_reg_update_byte_dt(
	            i2c, RV3028_REG_CONTROL_1, RV3028_CTRL1_EERD, pause ? RV3028_CTRL1_EERD : 0u) == 0)
	           ? ALP_OK
	           : ALP_ERR_IO;
}

/* CLKOUT low in the RAM mirror only: no EEPROM write cycle (100 cycles min at the
 * hot corner).  With EERD clear the chip reloads the mirror from EEPROM every 24 h,
 * which would switch CLKOUT back on mid-sleep, so EERD is set for the duration and
 * put back by rv3028_clkout_restore(). */
static alp_status_t rv3028_clkout_off(const struct i2c_dt_spec *i2c)
{
	if (i2c->bus == NULL || !device_is_ready(i2c->bus)) {
		return ALP_ERR_NOT_READY;
	}
	uint8_t v;
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_EE_CLKOUT, &v) != 0) {
		return ALP_ERR_IO;
	}
	v = (uint8_t)((v & ~RV3028_CLKOUT_FD_MASK) | RV3028_CLKOUT_FD_LOW);
	if (i2c_reg_write_byte_dt(i2c, RV3028_REG_EE_CLKOUT, v) != 0) {
		return ALP_ERR_IO;
	}
	return rv3028_eerd(i2c, true);
}

/* Whether the EEPROM refresh was already paused; false when it cannot be read. */
static bool rv3028_eerd_was_set(const struct i2c_dt_spec *i2c)
{
	uint8_t v = 0;

	if (i2c->bus == NULL || !device_is_ready(i2c->bus) ||
	    i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_1, &v) != 0) {
		return false;
	}
	return (v & RV3028_CTRL1_EERD) != 0u;
}

/* Put EERD back to what it was before the quiesce (@p prior), NOT unconditionally
 * clear: a caller that had paused the refresh for its own EEPROM work keeps it
 * paused.  CLKOUT itself stays low: nothing here needs it back. */
static alp_status_t rv3028_clkout_restore(const struct i2c_dt_spec *i2c, bool prior)
{
	return rv3028_eerd(i2c, prior);
}

/* ---- RV-3028 wake services (raw DT-I2C, no chip context needed) -------------
 *
 * The cold-boot wake decode runs before any application has bound a chip
 * context, so it reads the RV-3028 through the devicetree I2C address, like the
 * other default actions.  Register and flag layout: RV-3028-C7 Application
 * Manual Rev. 1.4 (STATUS 0Eh p.22, CONTROL_2 10h p.24, time 00h..06h p.17).
 * Arming the countdown goes through the chip driver instead (rtc hook,
 * som_power_rv3028.c): that procedure (App Manual Sec. 4.8.2) is not duplicated. */

#define RV3028_REG_SECONDS   0x00u
#define RV3028_REG_STATUS    0x0Eu
#define RV3028_REG_CONTROL_2 0x10u
#define RV3028_STATUS_PORF   0x01u
#define RV3028_STATUS_AF     0x04u
#define RV3028_STATUS_TF     0x08u
#define RV3028_STATUS_UF     0x10u
#define RV3028_STATUS_FLAGS  0x7Fu /* every latchable flag; EEbusy (bit7) is read-only */
#define RV3028_CTRL2_UIE     0x20u
#define RV3028_CTRL1_TE      0x04u
#define RV3028_CTRL2_AIE     0x08u
#define RV3028_CTRL2_TIE     0x10u

static const struct i2c_dt_spec *rtc_i2c(void)
{
	const struct i2c_dt_spec *i2c = &_reg[ALP_POWER_DOMAIN_RTC].i2c;

	if (!_reg[ALP_POWER_DOMAIN_RTC].present || i2c->bus == NULL || !device_is_ready(i2c->bus)) {
		return NULL;
	}
	return i2c;
}

alp_status_t alp_som_power_rtc_int_armed(bool *armed)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   c2  = 0;

	if (armed == NULL) {
		return ALP_ERR_INVAL;
	}
	*armed = false;
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_2, &c2) != 0) {
		return ALP_ERR_IO;
	}
	*armed = (c2 & (RV3028_CTRL2_AIE | RV3028_CTRL2_TIE)) != 0u;
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_porf(bool *porf)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   st  = 0;

	if (porf == NULL) {
		return ALP_ERR_INVAL;
	}
	*porf = false;
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_STATUS, &st) != 0) {
		return ALP_ERR_IO;
	}
	*porf = (st & RV3028_STATUS_PORF) != 0u;
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_regs(uint8_t regs[6])
{
	static const uint8_t      addr[6] = { 0x0Eu, 0x0Fu, 0x10u, 0x13u, 0x35u, 0x37u };
	const struct i2c_dt_spec *i2c     = rtc_i2c();

	if (regs == NULL) {
		return ALP_ERR_INVAL;
	}
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	for (unsigned i = 0; i < 6u; ++i) {
		if (i2c_reg_read_byte_dt(i2c, addr[i], &regs[i]) != 0) {
			return ALP_ERR_IO;
		}
	}
	return ALP_OK;
}

/* UF (the time-update flag) latches every second boundary of a running clock when nothing
 * clears it; with UIE off it does not drive /INT, but it is noise in every dump (bench U8g:
 * STATUS=0x10 at each refusal).  Clear it, and only it: a flag is cleared by writing 0, a 1 is
 * ignored, so every other bit is written as 1 and an event latching between the read and the
 * write survives.  Left alone when UIE is on (then it is somebody's event). */
alp_status_t alp_som_power_rtc_clear_stale_uf(void)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   st = 0, c2 = 0;

	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_STATUS, &st) != 0 ||
	    i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_2, &c2) != 0) {
		return ALP_ERR_IO;
	}
	if ((st & RV3028_STATUS_UF) == 0u || (c2 & RV3028_CTRL2_UIE) != 0u) {
		return ALP_OK;
	}
	return (i2c_reg_write_byte_dt(
	            i2c, RV3028_REG_STATUS, (uint8_t)(RV3028_STATUS_FLAGS & ~RV3028_STATUS_UF)) == 0)
	           ? ALP_OK
	           : ALP_ERR_IO;
}

/* Before this backend arms its own countdown: stop whatever an earlier cycle left running or
 * latched.  The RV-3028 is backup-powered, so a countdown / alarm that fired while nobody
 * handled it (an nRESET, a wake the decode never saw) keeps its enable and its flag across any
 * power cycle of the module, holds /INT low and refused every later sleep (#2784).  Stopped
 * here: the countdown (TE, TIE) and, unless @p keep_alarm (the caller may have armed its own
 * alarm), the alarm enable (AIE); cleared: TF, AF (not with keep_alarm) and, with UIE off, UF.
 * Each flag is cleared by writing 0 to it, every other bit is written as 1 (ignored by the
 * part), so PORF / EVF / BSF / CLKF survive: EVF with EIE on is the EVI wake (#2811). */
alp_status_t alp_som_power_rtc_clear_stale_wake(bool keep_alarm)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   c1 = 0, c2 = 0, st = 0, clear;

	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_1, &c1) != 0 ||
	    i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_2, &c2) != 0) {
		return ALP_ERR_IO;
	}
	/* Enables first, flags after, so nothing can re-latch behind the clear. */
	if ((c1 & RV3028_CTRL1_TE) != 0u &&
	    i2c_reg_write_byte_dt(i2c, RV3028_REG_CONTROL_1, (uint8_t)(c1 & ~RV3028_CTRL1_TE)) != 0) {
		return ALP_ERR_IO;
	}
	const uint8_t en = keep_alarm ? RV3028_CTRL2_TIE : (RV3028_CTRL2_TIE | RV3028_CTRL2_AIE);

	if ((c2 & en) != 0u &&
	    i2c_reg_write_byte_dt(i2c, RV3028_REG_CONTROL_2, (uint8_t)(c2 & ~en)) != 0) {
		return ALP_ERR_IO;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_STATUS, &st) != 0) {
		return ALP_ERR_IO;
	}
	clear = (uint8_t)(st & (keep_alarm ? RV3028_STATUS_TF : (RV3028_STATUS_TF | RV3028_STATUS_AF)));
	if ((st & RV3028_STATUS_UF) != 0u && (c2 & RV3028_CTRL2_UIE) == 0u) {
		clear |= RV3028_STATUS_UF;
	}
	if (clear == 0u) {
		return ALP_OK;
	}
	return (i2c_reg_write_byte_dt(
	            i2c, RV3028_REG_STATUS, (uint8_t)(RV3028_STATUS_FLAGS & ~clear)) == 0)
	           ? ALP_OK
	           : ALP_ERR_IO;
}

alp_status_t alp_som_power_rtc_flags_pending(bool *pending)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   st = 0, c2 = 0;

	if (pending == NULL) {
		return ALP_ERR_INVAL;
	}
	*pending = false;
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_STATUS, &st) != 0 ||
	    i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_2, &c2) != 0) {
		return ALP_ERR_IO;
	}
	*pending = ((st & RV3028_STATUS_TF) && (c2 & RV3028_CTRL2_TIE)) ||
	           ((st & RV3028_STATUS_AF) && (c2 & RV3028_CTRL2_AIE));
	return ALP_OK;
}

alp_status_t alp_som_power_rtc_wake_service(uint8_t *flags)
{
	const struct i2c_dt_spec *i2c = rtc_i2c();
	uint8_t                   st = 0, c2 = 0, hit;

	if (flags == NULL) {
		return ALP_ERR_INVAL;
	}
	*flags = 0u;
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_reg_read_byte_dt(i2c, RV3028_REG_STATUS, &st) != 0 ||
	    i2c_reg_read_byte_dt(i2c, RV3028_REG_CONTROL_2, &c2) != 0) {
		return ALP_ERR_IO;
	}
	/* A flag is a wake cause only with its interrupt enable on; UF latches every
	 * second regardless of UIE, so it is never one here. */
	*flags =
	    (uint8_t)(((st & RV3028_STATUS_TF) && (c2 & RV3028_CTRL2_TIE) ? RV3028_STATUS_TF : 0u) |
	              ((st & RV3028_STATUS_AF) && (c2 & RV3028_CTRL2_AIE) ? RV3028_STATUS_AF : 0u));
	hit = (uint8_t)(st & (RV3028_STATUS_TF | RV3028_STATUS_AF | RV3028_STATUS_UF));
	if (hit == 0u) {
		return ALP_OK; /* nothing latched: no STATUS write */
	}
	/* Acknowledge with a constant mask: 0 for the flags being cleared, 1 for every
	 * other latchable flag, so PORF / EVF / BSF / CLKF survive and a flag latching
	 * between the read and the write is not lost.  This relies on the part ignoring a
	 * 1 written to a flag that reads 0 (only a 0 clears): bench-verified 2026-10-08
	 * on E1M-AEN803 and documented at rv3028c7_wake_service() in
	 * include/alp/chips/rv3028c7.h (#2794). */
	if (i2c_reg_write_byte_dt(i2c, RV3028_REG_STATUS, (uint8_t)(RV3028_STATUS_FLAGS & ~hit)) != 0) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

static uint8_t rv_bcd(uint8_t v, bool *ok)
{
	if ((v & 0x0Fu) > 9u || (v >> 4) > 9u) {
		*ok = false;
	}
	return (uint8_t)(((v >> 4) * 10u) + (v & 0x0Fu));
}

/* Seconds since 2000-01-01 00:00:00 from the RV-3028 calendar (24 h mode, years
 * 2000..2099, so the only leap rule is year % 4). */
alp_status_t alp_som_power_rtc_seconds(uint32_t *seconds)
{
	static const uint16_t     cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	const struct i2c_dt_spec *i2c     = rtc_i2c();
	uint8_t                   r[7];
	uint8_t                   reg = RV3028_REG_SECONDS;
	bool                      ok  = true;

	if (seconds == NULL) {
		return ALP_ERR_INVAL;
	}
	*seconds = 0u;
	if (i2c == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (i2c_write_read_dt(i2c, &reg, 1, r, sizeof(r)) != 0) {
		return ALP_ERR_IO;
	}
	uint8_t sec = rv_bcd(r[0] & 0x7Fu, &ok);
	uint8_t min = rv_bcd(r[1] & 0x7Fu, &ok);
	uint8_t hr  = rv_bcd(r[2] & 0x3Fu, &ok);
	uint8_t day = rv_bcd(r[4] & 0x3Fu, &ok);
	uint8_t mon = rv_bcd(r[5] & 0x1Fu, &ok);
	uint8_t yr  = rv_bcd(r[6], &ok);

	if (!ok || sec > 59u || min > 59u || hr > 23u || day < 1u || day > 31u || mon < 1u ||
	    mon > 12u) {
		return ALP_ERR_IO;
	}
	uint32_t days = (uint32_t)yr * 365u + ((uint32_t)yr + 3u) / 4u + cum[mon - 1u] + (day - 1u);

	if (mon > 2u && (yr % 4u) == 0u) {
		days += 1u;
	}
	*seconds = days * 86400u + (uint32_t)hr * 3600u + (uint32_t)min * 60u + sec;
	return ALP_OK;
}

/* Countdown arming goes through the RV-3028 chip driver (som_power_rv3028.c,
 * built with CONFIG_ALP_SDK_CHIP_RV3028C7).  Without the driver these are the
 * truth: no countdown can be armed. */
__weak bool alp_som_power_rtc_countdown_ready(void)
{
	return false;
}

__weak alp_status_t alp_som_power_rtc_countdown_start(uint32_t seconds, uint32_t *actual_s)
{
	(void)seconds;
	(void)actual_s;
	return ALP_ERR_NOSUPPORT;
}

__weak alp_status_t alp_som_power_rtc_countdown_cancel(void)
{
	return ALP_ERR_NOSUPPORT;
}

const struct gpio_dt_spec *alp_som_power_wake_gpio(alp_power_domain_t d)
{
	if ((unsigned)d >= (unsigned)ALP_POWER_DOMAIN_COUNT || !_reg[d].present ||
	    _reg[d].wake.port == NULL) {
		return NULL;
	}
	return &_reg[d].wake;
}

/* ---- Default (no driver) actions ------------------------------------------- */

static bool prior_active(alp_power_domain_t d)
{
	const sompd_t *p = &_reg[d];
	if (p->action == SOMPD_A_ENABLE_LOW && p->enable.port != NULL &&
	    device_is_ready(p->enable.port)) {
		int v = gpio_pin_get_dt(&p->enable);
		return v != 0; /* a read error counts as "was on" */
	}
	if (d == ALP_POWER_DOMAIN_RTC) {
		return rv3028_eerd_was_set(&p->i2c); /* the saved bit is "EERD was set" */
	}
	return true;
}

alp_status_t alp_som_power_pin_quiesce(alp_power_domain_t d, bool rail_off)
{
	if ((unsigned)d >= (unsigned)ALP_POWER_DOMAIN_COUNT || !_reg[d].present) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	const sompd_t *p = &_reg[d];
	alp_status_t   s = ALP_OK;

	if (rail_off) {
		/* nRESET low BEFORE the supply, so the chip is held in reset while the
		 * rail collapses (same order as cc3501e_power_off()). */
		s = pin_assert(&p->reset, true);
		if (s == ALP_OK) {
			s = pin_assert(&p->enable, false);
		}
		_assert_ms[d] = k_uptime_get();
		return s;
	}

	switch ((sompd_action_t)p->action) {
	case SOMPD_A_HOLD_RESET:
		s             = pin_assert(&p->reset, true);
		_assert_ms[d] = k_uptime_get();
		return s;
	case SOMPD_A_POWERDOWN_PIN:
		return pin_assert(&p->powerdown, true);
	case SOMPD_A_ENABLE_LOW:
		return pin_assert(&p->enable, false);
	case SOMPD_A_SHUTDOWN_REG:
		return tmp112_shutdown(&p->i2c, true);
	case SOMPD_A_KEEP_ALIVE:
		/* Never powered down; only the RTC's CLKOUT goes quiet. */
		return (d == ALP_POWER_DOMAIN_RTC) ? rv3028_clkout_off(&p->i2c) : ALP_OK;
	case SOMPD_A_DEEP_POWER_DOWN_CMD:
	default:
		return ALP_ERR_NOSUPPORT;
	}
}

alp_status_t alp_som_power_pin_restore(alp_power_domain_t d, bool rail_off, bool early, bool prior)
{
	if ((unsigned)d >= (unsigned)ALP_POWER_DOMAIN_COUNT || !_reg[d].present) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	const sompd_t *p = &_reg[d];
	alp_status_t   s;

	if (rail_off) {
		s = pin_assert(&p->enable, true);
		if (s != ALP_OK) {
			return s;
		}
		wait_ms(SOMPD_RAIL_UP_MS, early);
	}

	switch ((sompd_action_t)p->action) {
	case SOMPD_A_HOLD_RESET:
		/* cc3501e_hard_reset() semantics: nRESET low for >= 50 ms, release, rails
		 * untouched.  The line has normally been low for far longer; top up only
		 * when a RUN-mode cycle was shorter than the pulse. */
		if (!early) {
			int64_t held = k_uptime_get() - _assert_ms[d];
			if (held < (int64_t)SOMPD_NRST_MIN_HOLD_MS) {
				k_msleep((int32_t)(SOMPD_NRST_MIN_HOLD_MS - (uint32_t)held));
			}
		}
		s = pin_assert(&p->reset, false);
		if (s != ALP_OK) {
			return s;
		}
		if (d == ALP_POWER_DOMAIN_WIFI_BLE) {
			/* BLIND boot settle: the CC3501E must not be clocked until its SPI
			 * slave is armed.  Seconds long, so the cold-boot path skips it and
			 * lets the bring-up code wait; a runtime restore waits here. */
			if (!early) {
				k_msleep(CONFIG_ALP_SDK_SOM_PD_WIFI_SETTLE_MS);
			}
		} else {
			wait_ms(SOMPD_MEM_RESET_MS, early);
		}
		return ALP_OK;
	case SOMPD_A_POWERDOWN_PIN:
		s = pin_assert(&p->powerdown, false);
		if (s != ALP_OK) {
			return s;
		}
		wait_ms(SOMPD_PHY_PWRUP_MS, early);
		if (p->reset.port != NULL) {
			/* A PHY that just regained power needs a reset pulse to come up in a
			 * known state.  A reset pad that is not usable here is not fatal. */
			if (pin_assert(&p->reset, true) == ALP_OK) {
				wait_ms(SOMPD_PHY_RESET_MS, early);
				(void)pin_assert(&p->reset, false);
			}
		}
		wait_ms(early ? SOMPD_PHY_SETTLE_EARLY : SOMPD_PHY_SETTLE_MS, early);
		return ALP_OK;
	case SOMPD_A_ENABLE_LOW:
		return pin_assert(&p->enable, true);
	case SOMPD_A_SHUTDOWN_REG:
		return tmp112_shutdown(&p->i2c, false);
	case SOMPD_A_KEEP_ALIVE:
		return (d == ALP_POWER_DOMAIN_RTC) ? rv3028_clkout_restore(&p->i2c, prior) : ALP_OK;
	case SOMPD_A_DEEP_POWER_DOWN_CMD:
	default:
		return ALP_ERR_NOSUPPORT;
	}
}

/* ---- Hook dispatch --------------------------------------------------------- */

static alp_status_t run_quiesce(alp_power_domain_t d, bool rail_off)
{
	if (_hooks[d] != NULL && _hooks[d]->quiesce != NULL) {
		return _hooks[d]->quiesce(_hook_ctx[d], rail_off);
	}
	return alp_som_power_pin_quiesce(d, rail_off);
}

static alp_status_t run_restore(alp_power_domain_t d, bool rail_off, bool early, bool prior)
{
	if (_hooks[d] != NULL && _hooks[d]->restore != NULL) {
		return _hooks[d]->restore(_hook_ctx[d], rail_off, early);
	}
	if (_reg[d].action == SOMPD_A_ENABLE_LOW && !prior) {
		return ALP_OK; /* it was off before the quiesce: leave it off */
	}
	return alp_som_power_pin_restore(d, rail_off, early, prior);
}

alp_status_t alp_som_power_bind(alp_power_domain_t d, const alp_som_power_hooks_t *hooks, void *ctx)
{
	if ((unsigned)d >= (unsigned)ALP_POWER_DOMAIN_COUNT || hooks == NULL) {
		return ALP_ERR_INVAL;
	}
	k_mutex_lock(&_lock, K_FOREVER);
	alp_status_t s = ALP_OK;
	if (_state[d] == ALP_SOM_PD_QUIESCED) {
		s = ALP_ERR_BUSY;
	} else {
		_hooks[d]    = hooks;
		_hook_ctx[d] = ctx;
	}
	k_mutex_unlock(&_lock);
	return s;
}

void *alp_som_power_bound_ctx(alp_power_domain_t d)
{
	return ((unsigned)d < (unsigned)ALP_POWER_DOMAIN_COUNT && _hooks[d] != NULL) ? _hook_ctx[d]
	                                                                             : NULL;
}

void alp_som_power_unbind(alp_power_domain_t d)
{
	if ((unsigned)d >= (unsigned)ALP_POWER_DOMAIN_COUNT) {
		return;
	}
	k_mutex_lock(&_lock, K_FOREVER);
	_hooks[d]    = NULL;
	_hook_ctx[d] = NULL;
	k_mutex_unlock(&_lock);
}

/* This core's (M55-HE or M55-HP) reset syndrome, read and acknowledged (strong definition:
 * alif_se_power_hw.c).  Without it every boot looks like a Secure-Enclave-initiated one. */
__weak uint32_t alp_som_power_reset_syndrome_take(void)
{
	return 0u;
}

__weak bool alp_som_power_reset_syndrome_trusted(void)
{
	return false;
}

/* This core's WIC bits say it was powered off (strong definition: alif_se_power_hw.c).  Read and
 * cleared on every boot. */
__weak bool alp_som_power_core_off_take(void)
{
	return false;
}

/* RESETSYNDROME bit 2: a reset request to the power domain (E8 SVD AON.RTSS_x_RESET). */
#define SOMPD_RESET_PD_REQUEST BIT(2)

/* RESETSYNDROME bit 0: the NSRST pin was asserted (E8 SVD AON.RTSS_HE_RESET / RTSS_HP_RESET). */
#define SOMPD_RESET_NSRST BIT(0)

/* ---- Wake decode hooks ------------------------------------------------------ */

/* The STOP backend (alif_se_power.c) overrides these.  Without it the record's
 * wake_source / slept_ms stay as stored (zero), which is the truth: nothing else
 * knows why the SoC woke. */
__weak void alp_som_power_wake_decode_early(alp_som_pd_record_t *rec)
{
	(void)rec;
}

__weak bool alp_som_power_wake_decode_i2c(alp_som_pd_record_t *rec)
{
	(void)rec;
	return true;
}

/* ---- Quiesce / restore ----------------------------------------------------- */

/* Domains whose action goes over BRD_I2C.  The cold-boot restore handles them in a
 * second pass once the I2C controller has initialised. */
#define SOMPD_I2C_DOMAINS \
	(ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_TEMP_SENSOR) | \
	 ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_RTC))

/* Restore the domains of @p rec that are also in @p only, last-quiesced first.
 * Never stops on a failure. */
static void restore_domains(const alp_som_pd_record_t *rec,
                            uint32_t                   only,
                            bool                       early,
                            uint32_t                  *restored,
                            uint32_t                  *failed)
{
	*restored = 0u;
	*failed   = 0u;
	for (int i = (int)ALP_POWER_DOMAIN_COUNT - 1; i >= 0; --i) {
		alp_power_domain_t d   = _order[i];
		uint32_t           bit = ALP_POWER_DOMAIN_BIT(d);
		if ((rec->quiesced & only & bit) == 0u) {
			continue;
		}
		alp_status_t s =
		    run_restore(d, (rec->rail_off & bit) != 0u, early, (rec->prior_active & bit) != 0u);
		if (s == ALP_OK) {
			_state[d] = ALP_SOM_PD_ACTIVE;
			*restored |= bit;
		} else {
			printk("som_power: restore of domain %d failed, status %d\n", (int)d, (int)s);
			_state[d] = ALP_SOM_PD_RESTORE_FAILED;
			*failed |= bit;
		}
	}
}

alp_status_t alp_som_power_quiesce(alp_power_mode_t mode, uint32_t *rollback_failed)
{
	if (rollback_failed != NULL) {
		*rollback_failed = 0u;
	}
	if (mode == ALP_POWER_MODE_SLEEP || mode == ALP_POWER_MODE_DEEP_SLEEP) {
		return ALP_OK; /* v1: only STOP / STANDBY quiesce */
	}
	if (mode != ALP_POWER_MODE_RUN && mode != ALP_POWER_MODE_STOP &&
	    mode != ALP_POWER_MODE_STANDBY) {
		return ALP_ERR_INVAL;
	}

	k_mutex_lock(&_lock, K_FOREVER);
	alp_som_pd_record_t rec = { 0 };
	alp_status_t        s   = ALP_OK;

	for (size_t i = 0; i < ALP_POWER_DOMAIN_COUNT && s == ALP_OK; ++i) {
		alp_power_domain_t d   = _order[i];
		const sompd_t     *p   = &_reg[d];
		uint32_t           bit = ALP_POWER_DOMAIN_BIT(d);

		if (!p->present || _policy[d] == ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE) {
			continue;
		}
		if (mode != ALP_POWER_MODE_RUN && (p->modes & BIT(mode)) == 0u) {
			continue;
		}
		if (_state[d] == ALP_SOM_PD_QUIESCED) {
			s = ALP_ERR_BUSY;
			break;
		}
		bool rail  = (_policy[d] == ALP_POWER_DOMAIN_POLICY_RAIL_OFF);
		bool prior = prior_active(d);
		s          = run_quiesce(d, rail);
		if (s != ALP_OK) {
			printk("som_power: quiesce of domain %d failed, status %d\n", (int)d, (int)s);
		}
		/* Record the domain even when its quiesce failed: a multi-step action
		 * (rail-off drives nRESET, then the supply) may have completed its first
		 * step, and the rollback below must undo that too.  Releasing a pad that
		 * was never driven is harmless. */
		_state[d] = ALP_SOM_PD_QUIESCED;
		rec.quiesced |= bit;
		if (rail) {
			rec.rail_off |= bit;
		}
		if (prior) {
			rec.prior_active |= bit;
		}
	}

	if (s != ALP_OK) {
		/* Put back everything touched, including the failing domain's own partial
		 * step; the caller sees the error and must not enter the sleep.  A domain
		 * whose rollback also fails stays RESTORE_FAILED, is reported through
		 * @p rollback_failed, and is kept in the record so a later
		 * alp_som_power_restore() can retry it. */
		uint32_t restored, failed;
		restore_domains(&rec, ~0u, false, &restored, &failed);
		if (failed != 0u) {
			rec.quiesced = failed;
			/* RUN, not @p mode: the retry record must survive a warm reset
			 * that STOP_MODE_STAT (0 on a warm reset) cannot vouch for;
			 * leaving a rollback-failed domain held would be the worse outcome. */
			rec.mode = (uint32_t)ALP_POWER_MODE_RUN;
			alp_som_pd_store_save(&rec);
			_ram_rec       = rec;
			_ram_rec_valid = true;
		}
		if (rollback_failed != NULL) {
			*rollback_failed = failed;
		}
	} else if (rec.quiesced != 0u) {
		rec.mode = (uint32_t)mode;
		alp_som_pd_store_save(&rec);
		_ram_rec       = rec;
		_ram_rec_valid = true;
	}
	k_mutex_unlock(&_lock);
	return s;
}

alp_status_t alp_som_power_restore(uint32_t *failed_out)
{
	k_mutex_lock(&_lock, K_FOREVER);
	alp_som_pd_record_t rec;
	if (!alp_som_pd_store_load(&rec)) {
		/* BKRAM did not give the record back (dead, or not retained after the SE call):
		 * the in-RAM copy of the same quiesce is the truth for a same-boot rollback. */
		if (!_ram_rec_valid) {
			k_mutex_unlock(&_lock);
			return ALP_ERR_NOT_READY;
		}
		rec = _ram_rec;
	}
	_ram_rec_valid = false;
	uint32_t restored, failed;
	restore_domains(&rec, ~0u, false, &restored, &failed);
	alp_som_pd_store_clear();
	if (failed != 0u) {
		/* As the quiesce rollback does: a domain that could not be put back stays in the
		 * record (RUN mode, so the next boot's restore does not need STOP_MODE_STAT to
		 * vouch for it), in BKRAM and in RAM, so a retry still has the data. */
		alp_som_pd_record_t retry = { .mode         = (uint32_t)ALP_POWER_MODE_RUN,
			                          .quiesced     = failed,
			                          .rail_off     = rec.rail_off & failed,
			                          .prior_active = rec.prior_active & failed };

		alp_som_pd_store_save(&retry);
		_ram_rec       = retry;
		_ram_rec_valid = true;
	}
	k_mutex_unlock(&_lock);
	if (failed_out != NULL) {
		*failed_out = failed;
	}
	return (failed == 0u) ? ALP_OK : ALP_ERR_IO;
}

alp_status_t alp_som_power_cycle_run(uint32_t hold_ms, uint32_t *failed)
{
	if (failed != NULL) {
		*failed = 0u;
	}
	alp_status_t s = alp_som_power_quiesce(ALP_POWER_MODE_RUN, failed);
	if (s != ALP_OK || alp_som_power_quiesced() == 0u) {
		return s;
	}
	k_msleep((int32_t)hold_ms);
	return alp_som_power_restore(failed);
}

alp_som_pd_state_t alp_som_power_state(alp_power_domain_t d)
{
	return ((unsigned)d < (unsigned)ALP_POWER_DOMAIN_COUNT) ? _state[d] : ALP_SOM_PD_ACTIVE;
}

uint32_t alp_som_power_quiesced(void)
{
	uint32_t mask = 0u;
	for (size_t d = 0; d < ALP_POWER_DOMAIN_COUNT; ++d) {
		if (_state[d] == ALP_SOM_PD_QUIESCED) {
			mask |= ALP_POWER_DOMAIN_BIT(d);
		}
	}
	return mask;
}

alp_power_domain_policy_t alp_som_power_policy(alp_power_domain_t d)
{
	return ((unsigned)d < (unsigned)ALP_POWER_DOMAIN_COUNT) ? _policy[d]
	                                                        : ALP_POWER_DOMAIN_POLICY_AUTO;
}

void alp_som_power_reset_for_test(void)
{
	k_mutex_lock(&_lock, K_FOREVER);
	memset(_policy, 0, sizeof(_policy));
	memset(_state, 0, sizeof(_state));
	memset(_hooks, 0, sizeof(_hooks));
	memset(_hook_ctx, 0, sizeof(_hook_ctx));
	memset(&_boot, 0, sizeof(_boot));
	memset(&_boot_rec, 0, sizeof(_boot_rec));
	_boot_external   = false;
	_boot_suppressed = false;
	_ignore_stat     = false;
	_pads_applied    = false;
	_ram_rec_valid   = false;
	alp_som_pd_store_clear();
	k_mutex_unlock(&_lock);
}

/* ---- Cold-boot restore ------------------------------------------------------ */

/* STOP_MODE_STAT (0x1A60F000 bit4) must agree that the last reset was a STOP
 * wake.  Absent register (non-Alif build) -> no extra evidence to require. */
#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(stop_mode))
#define SOMPD_STOP_MODE_STAT_BIT BIT(4)
/* Weak so the host tests can stand in for the register. */
__weak uint32_t alp_som_power_stop_mode_read(void)
{
	return sys_read32(DT_REG_ADDR(DT_NODELABEL(stop_mode)));
}
/* The register is present and says "this boot is a STOP wake". */
static bool stop_mode_stat_set(void)
{
	return !_ignore_stat && (alp_som_power_stop_mode_read() & SOMPD_STOP_MODE_STAT_BIT) != 0u;
}
static bool stop_mode_stat_agrees(void)
{
	return stop_mode_stat_set();
}
#else
static bool stop_mode_stat_set(void)
{
	return false; /* no register: a record-less boot is a plain POR */
}
static bool stop_mode_stat_agrees(void)
{
	return true;
}
#endif

/* A STOP wake whose record is missing or fails its CRC (the SRAM did not hold, the
 * record was never written, a corrupted write): the domains it would have named are
 * unknown, so nothing may be left held.  Release every present control pad to its
 * inactive level, as if the whole set had been quiesced with AUTO (no rail-off, no
 * saved "was on" bit, so the backlight stays off).  Reported as a STOP cycle with
 * no wake cause. */
static void blind_restore(uint32_t *restored, uint32_t *failed, uint32_t *named)
{
	alp_som_pd_record_t rec = { .mode = (uint32_t)ALP_POWER_MODE_STOP };

	for (size_t d = 0; d < ALP_POWER_DOMAIN_COUNT; ++d) {
		if (_reg[d].present) {
			rec.quiesced |= ALP_POWER_DOMAIN_BIT(d);
			_state[d] = ALP_SOM_PD_QUIESCED;
		}
	}
	restore_domains(&rec, ~SOMPD_I2C_DOMAINS, true, restored, failed);
	*named = rec.quiesced;
}

int alp_som_power_boot_restore(void)
{
	k_mutex_lock(&_lock, K_FOREVER);
	memset(&_boot, 0, sizeof(_boot));
	/* A pin reset (a debugger nRESET, a reset button) during or after a sleep also finds
	 * STOP_MODE_STAT set and a valid record, and used to be reported as a wake with no
	 * cause.  It is not one: the sleep was cut short from outside.  The domains still
	 * have to be put back, so the restore below is unchanged; only the report differs. */
	const uint32_t syndrome = alp_som_power_reset_syndrome_take();
	const bool     nsrst    = (syndrome & SOMPD_RESET_NSRST) != 0u;
	/* Second STOP witness, consumed on every path: with another core running the DC-DC
	 * stays up and STOP_MODE_STAT never sets, but this core's WIC "subsystem off" bits
	 * survive (bench run D, E1M-AEN803 2026W36-0001). */
	const bool core_off = alp_som_power_core_off_take();

	_boot_external   = false;
	_ignore_stat     = false;
	_boot_suppressed = false;

	/* Another image's data in BKRAM (a clean flash on top of a run that slept, bench U8d):
	 * the counter and the diag belong to someone else, and a sticky STOP_MODE_STAT set by
	 * that run is not this image's wake.  Start fresh, ignore the status and report nothing
	 * this boot -- but STILL put back whatever that run left held (from its record, or
	 * blind): a domain left in reset is the worse outcome. */
	bool foreign_cell = false;
	bool foreign_rec  = false;

	if (alp_som_pd_bkram_foreign()) {
		alp_som_pd_bkram_adopt();
		_ignore_stat     = true;
		_boot_suppressed = true;
		foreign_cell     = true;
	}

	alp_som_pd_diag_patch(ALP_SOM_PD_DIAG_BOOT, 51u, alp_som_pd_image_id());

	alp_som_pd_record_t rec;
	bool                have = alp_som_pd_store_load(&rec);

	if (have && rec.image_id != alp_som_pd_image_id()) {
		alp_som_pd_bkram_adopt();
		_ignore_stat     = true;
		_boot_suppressed = true;
		foreign_rec      = true; /* restored from its record below, but not reported */
	}
	if (!have) {
		if (stop_mode_stat_set() || core_off || foreign_cell) {
			/* A STOP wake with no usable record: never leave NOR, PHY or the CC3501E held. */
			uint32_t restored, failed, named;

			blind_restore(&restored, &failed, &named);
			memset(&_boot_rec, 0, sizeof(_boot_rec));
			_boot_rec.mode = (uint32_t)ALP_POWER_MODE_STOP;
			_boot.valid    = true;
			_boot.mode     = (uint32_t)(_boot_external ? ALP_POWER_MODE_RUN : ALP_POWER_MODE_STOP);
			_boot.quiesced = named;
			_boot.restored = restored;
			_boot.failed   = failed;
		}
		/* Otherwise a plain POR: touch nothing. */
		alp_som_pd_store_clear();
		k_mutex_unlock(&_lock);
		return 0;
	}
	/* Only a STOP record is vouched for by STOP_MODE_STAT.  A RUN-mode record is the
	 * bench / test cycle interrupted by a warm reset, and a STANDBY record's status
	 * bit is not established (the register names STOP only): for both, leaving the
	 * domains held would be the worse outcome, so they are restored. */
	if (!foreign_rec && rec.mode == (uint32_t)ALP_POWER_MODE_STOP && !core_off &&
	    !stop_mode_stat_agrees()) {
		alp_som_pd_store_clear();
		k_mutex_unlock(&_lock);
		return 0;
	}

	/* The in-RAM state is fresh after a cold boot; the record is the truth. */
	for (size_t d = 0; d < ALP_POWER_DOMAIN_COUNT; ++d) {
		if ((rec.quiesced & ALP_POWER_DOMAIN_BIT(d)) != 0u) {
			_state[d] = ALP_SOM_PD_QUIESCED;
		}
	}
	/* Pass 1: everything that needs only a GPIO pad.  The I2C-backed domains wait
	 * for pass 2 (alp_som_power_boot_restore_i2c) -- the controller is not up yet. */
	uint32_t restored, failed;
	restore_domains(&rec, ~SOMPD_I2C_DOMAINS, true, &restored, &failed);

	/* A set NSRST bit marks a pin reset ONLY if it was probed before the sleep and does
	 * clear (ALP_SOM_REC_NSRST_TRUSTED in the record): the SVD calls the field write-only
	 * with reset value 1, and a stale bit must never turn a genuine wake into an aborted
	 * sleep. */
	_boot_external = nsrst && (rec.armed_hw & ALP_SOM_REC_NSRST_TRUSTED) != 0u;
	/* core_off as the only witness: a power-domain reset request (syndrome bit2) means
	 * the entry was cut short from outside. */
	if (core_off && !stop_mode_stat_set() && (syndrome & SOMPD_RESET_PD_REQUEST) != 0u) {
		_boot_external = true;
	}

	/* Wake cause, part 1: what needs no I2C (LPTIMER status).  This must run before
	 * the timer driver initialises and clears the status; the RTC half follows in
	 * the I2C pass. */
	_boot_rec = rec;
	alp_som_power_wake_decode_early(&_boot_rec);

	_boot.valid       = true;
	_boot.mode        = _boot_rec.mode;
	_boot.wake_source = _boot_rec.wake_source;
	_boot.slept_ms    = _boot_rec.slept_ms;
	if (_boot_external && rec.mode != (uint32_t)ALP_POWER_MODE_RUN) {
		/* Aborted sleep / external reset, not a wake: the realised mode is RUN and
		 * there is no wake cause or sleep time. */
		_boot.mode        = (uint32_t)ALP_POWER_MODE_RUN;
		_boot.wake_source = 0u;
		_boot.slept_ms    = 0u;
	}
	_boot.quiesced = rec.quiesced;
	_boot.restored = restored;
	_boot.failed   = failed;

	alp_som_pd_store_clear();
	k_mutex_unlock(&_lock);
	return 0;
}

/* STOP_MODE_STAT is sticky: left set, a later reset of any kind (a flash, a debugger
 * nRESET) reads as a STOP wake.  Acknowledged once the boot decode is done.  Weak: true
 * where there is no such register. */
__weak bool alp_som_power_stop_mode_stat_clear(void)
{
	return true;
}

int alp_som_power_boot_restore_i2c(void)
{
	k_mutex_lock(&_lock, K_FOREVER);
	if (_boot.valid) {
		alp_som_pd_record_t rec = { .quiesced = _boot.quiesced & SOMPD_I2C_DOMAINS };
		uint32_t            restored, failed;

		restore_domains(&rec, SOMPD_I2C_DOMAINS, true, &restored, &failed);
		_boot.restored |= restored;
		_boot.failed |= failed;

		/* Wake cause, part 2: the RV-3028 flags and the slept time. */
		if (alp_som_power_wake_decode_i2c(&_boot_rec)) {
			if (!_boot_external || _boot_rec.mode == (uint32_t)ALP_POWER_MODE_RUN) {
				_boot.wake_source = _boot_rec.wake_source;
				_boot.slept_ms    = _boot_rec.slept_ms;
			} /* else: an external reset has no wake cause (decode only acknowledged flags) */
		} else {
			memset(&_boot, 0, sizeof(_boot)); /* untrusted record: report a plain boot */
		}
	}
	/* The decode is done (pass 1 read the status, pass 2 the RTC): acknowledge the status. */
	(void)alp_som_power_stop_mode_stat_clear();
	k_mutex_unlock(&_lock);
	return 0;
}

/* Pass 1 runs right after the GPIO controllers (PRE_KERNEL_1) and before the
 * flash, Ethernet and sensor drivers.  Pass 2 needs the BRD_I2C controller, which
 * i2c_dw brings up at POST_KERNEL CONFIG_I2C_INIT_PRIORITY, so it must run after
 * that and still before the sensor and Ethernet drivers that use the bus and the
 * PHY clock.  SYS_INIT needs a literal priority, hence the assertions. */
#define SOMPD_I2C_RESTORE_PRIO 51
#ifdef CONFIG_I2C_INIT_PRIORITY
BUILD_ASSERT(SOMPD_I2C_RESTORE_PRIO > CONFIG_I2C_INIT_PRIORITY,
             "som_power: the I2C-backed restore must run after the I2C controller initialises");
#endif
#ifdef CONFIG_ETH_INIT_PRIORITY
BUILD_ASSERT(SOMPD_I2C_RESTORE_PRIO < CONFIG_ETH_INIT_PRIORITY,
             "som_power: the I2C-backed restore must run before the Ethernet driver");
#endif
#ifdef CONFIG_SENSOR_INIT_PRIORITY
BUILD_ASSERT(SOMPD_I2C_RESTORE_PRIO < CONFIG_SENSOR_INIT_PRIORITY,
             "som_power: the I2C-backed restore must run before the sensor drivers");
#endif
#ifdef CONFIG_FLASH_INIT_PRIORITY
BUILD_ASSERT(
    CONFIG_FLASH_INIT_PRIORITY > 1,
    "som_power: the pin restore (and the flash hook binding at 1) precede the flash driver");
#endif

SYS_INIT(alp_som_power_boot_restore, POST_KERNEL, 0);
SYS_INIT(alp_som_power_boot_restore_i2c, POST_KERNEL, 51);

/* ---- Power-class op wrappers ------------------------------------------------ */

static bool rail_off_available(alp_power_domain_t d)
{
	const sompd_t *p = &_reg[d];
	return IS_ENABLED(CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF) && d == ALP_POWER_DOMAIN_WIFI_BLE &&
	       p->rail_off_opt_in && p->enable.port != NULL;
}

alp_status_t alp_som_power_ops_policy_set(alp_power_backend_state_t *state,
                                          alp_power_domain_t         domain,
                                          alp_power_domain_policy_t  policy)
{
	(void)state;
	if (!_reg[domain].present) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	if (policy == ALP_POWER_DOMAIN_POLICY_RAIL_OFF && !rail_off_available(domain)) {
		return ALP_ERR_NOSUPPORT;
	}
	k_mutex_lock(&_lock, K_FOREVER);
	alp_status_t s = ALP_OK;
	if (_state[domain] == ALP_SOM_PD_QUIESCED) {
		s = ALP_ERR_BUSY; /* the held domain was taken under the old policy */
	} else {
		_policy[domain] = policy;
	}
	k_mutex_unlock(&_lock);
	return s;
}

static uint32_t action_bit(const sompd_t *p, sompd_action_t a)
{
	switch (a) {
	case SOMPD_A_HOLD_RESET:
		return (p->reset.port != NULL) ? ALP_POWER_ACTION_HOLD_RESET : 0u;
	case SOMPD_A_POWERDOWN_PIN:
		return (p->powerdown.port != NULL) ? ALP_POWER_ACTION_POWERDOWN_PIN : 0u;
	case SOMPD_A_ENABLE_LOW: /* the enable pad driven to its quiesce level */
		return (p->enable.port != NULL) ? ALP_POWER_ACTION_POWERDOWN_PIN : 0u;
	case SOMPD_A_SHUTDOWN_REG:
		return (p->i2c.bus != NULL) ? ALP_POWER_ACTION_SHUTDOWN_REG : 0u;
	case SOMPD_A_DEEP_POWER_DOWN_CMD: /* no driver exposes it yet */
	case SOMPD_A_KEEP_ALIVE:
	default:
		return 0u;
	}
}

alp_status_t alp_som_power_ops_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out)
{
	const sompd_t *p = &_reg[domain];
	if (!p->present) {
		return ALP_OK; /* zero-filled: present = false */
	}
	out->present            = true;
	out->holds_through_stop = p->holds_through_stop;
	out->default_action     = action_bit(p, (sompd_action_t)p->action);
	out->supported_actions  = out->default_action;
	if (rail_off_available(domain)) {
		out->supported_actions |= ALP_POWER_ACTION_RAIL_OFF;
	}
	out->dependents = p->dependents;
	return ALP_OK;
}

alp_status_t alp_som_power_ops_boot_wake_info(alp_power_boot_info_t *out)
{
	if (_boot_suppressed) {
		return ALP_OK; /* another image's leftovers were cleaned up; nothing to report */
	}
	out->valid                  = _boot.valid;
	out->realised_mode          = (alp_power_mode_t)_boot.mode;
	out->wake_source            = _boot.wake_source;
	out->slept_ms               = _boot.slept_ms;
	out->quiesced_domains       = _boot.quiesced;
	out->restored_domains       = _boot.restored;
	out->restore_failed_domains = _boot.failed;
	return ALP_OK;
}
