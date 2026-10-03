/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin Zephyr glue over the vendored r_wdt FSP module) -- BENCH-UNVERIFIED ======
 * Binds the RZ/V2N Cortex-M33 watchdog (WDT0, R_WDT0_BASE 0x41C00400) to the Zephyr watchdog API so
 * <alp/wdt.h> works through the portable src/backends/wdt/zephyr_drv.c with no new public API.
 *
 * Why not the upstream drivers/watchdog/wdt_renesas_rz.c (`renesas,rz-wdt`): checked against Zephyr
 * v4.4.1 + hal_renesas 06282060fa, it cannot bind this SoC.  hal_renesas has no rzv r_wdt module at
 * all (r_wdt exists for rza/rzn/rzt only), and the upstream driver has exactly two builds -- the
 * RZ/A overflow-timer build (wdt_extended_cfg_t / wdt_overflow_isr, a different register set) and the
 * WDT_RENESAS_RZ_USE_ICU build (R_ICU->PERIERR_*, R_SYSC_NS, SWRCPU0 -- RZ/T2M and RZ/N2L only, the
 * Kconfig depends on those series).  RZ/V2N WDT0 has the rzt/rzn register layout but neither the ICU
 * nor those SYSC registers, so neither build compiles.  A distinct compatible (`renesas,rzv-wdt`) keeps
 * this from ever colliding with a real `renesas,rz-wdt` node and leaves `west update` clean.  The
 * timeout-selection idea is the upstream USE_ICU build's, minus the window/ICU parts.
 *
 * Behaviour (RZ/V2N hardware manual R01UH1071EJ0120 Rev.1.20 = "the manual"):
 *   - reset-only: WDT_FLAG_RESET_SOC.  WDTRCR.RSTIRQS is 0 (manual 5.4.2.2.4: "In this LSI, always set
 *     RSTIRQS = 0 when using the WDT"), so an underflow raises WDT_CM33_iwdt_nmiundf_n, which
 *     R_BSP_WDT_SYSTEM_RESET_ENABLE(0, 1) (CPG_ERRORRST_SEL2.ERRRSTSEL0, manual 4.4.4.14 + Table 4.4-29)
 *     turns into an error SYSTEM reset: the CM33, CA55, CoreSight and all other units reset (manual
 *     4.4.6.5 reset table, "System reset triggered by various errors"), i.e. the whole SoM.
 *   - WDT_FLAG_RESET_CPU_CORE is -ENOTSUP.  With SEL2 = 0 the underflow is NOT a CM33-only reset: the
 *     only CM33-only path is CPG_ERRORRST_SEL1.ERRRSTSEL2 (manual 4.4.4.13, "CM33 cold reset", hal
 *     R_BSP_WDT_COLD_RESET_ENABLE), which answers a WDT *reset request* -- the RSTIRQS = 1 output the
 *     manual forbids on this LSI.  Whether it works with RSTIRQS = 0 is not documented, so it is not
 *     offered until a bench run shows it (see docs/bench/rzv-wdt0-cm33.md).
 *   - interrupt-only (callback) is -ENOTSUP: hal_renesas rzv2n names no WDT NVIC line for the CM33
 *     (manual Table 5.4-5: WDT0 "Interrupt to CPU: Not possible", error interrupt only).
 *   - no window: window.min must be 0 (the hardware window is fixed at 0..100 %).
 *   - the achieved timeout is the longest period <= window.max (never longer than asked); at 24 MHz
 *     that tops out at 16384 x 256 / 24 MHz = 174.8 ms (wdt_rzv_ceiling_ms(), 175 ms).  A longer request
 *     runs EXTENDED mode: the hardware is armed with a fixed 87.4 ms period and a k_timer keeper (20 ms,
 *     system-timer ISR) refreshes it only while the app deadline (window.max ms since the last
 *     wdt_feed, 64-bit uptime, spinlock-guarded) has not passed.  After a missed deadline the keeper
 *     stops and the hardware resets the SoM: reset lands in [timeout, timeout + 87.4 ms] after the
 *     last feed.  A kernel/IRQ lockup stops the keeper too, so it still resets within 87.4 ms.  In
 *     extended mode wdt_feed only stamps the deadline (the keeper is the sole hardware refresher,
 *     the refresh being a two-write sequence).  The clock
 *     is the node's `clock-freq`, generated from the SoC spec's m33_sm `watchdog.counting_clock_hz`:
 *     24 MHz = WDT_0_clk_loco from the Main OSC (manual Table 4.4-2, "CWDT loco clock").
 *   - WDT_OPT_PAUSE_IN_SLEEP maps to WDTCSTPR.SLCSTP; WDT_OPT_PAUSE_HALTED_BY_DBG is -ENOTSUP.
 *   - once started the watchdog cannot be stopped (wdt_disable() -> -EPERM), as on the other RZ WDTs.
 * Not verified on silicon: see docs/bench/rzv-wdt0-cm33.md for the ordered bench steps.
 * ============================================================================
 */

#define DT_DRV_COMPAT renesas_rzv_wdt

#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "r_wdt.h"
#include "wdt_rzv_timeout.h"

LOG_MODULE_REGISTER(wdt_renesas_rzv, CONFIG_WDT_LOG_LEVEL);

#define WDT_RZV_ATOMIC_ENABLE      0
#define WDT_RZV_ATOMIC_TIMEOUT_SET 1

struct wdt_rzv_config {
	uint32_t clock_freq;
};

struct wdt_rzv_data {
	wdt_instance_ctrl_t ctrl;
	wdt_cfg_t           cfg;
	atomic_t            state;
	/* Extended mode (request above the hardware ceiling): the keeper owns the hardware refresh. */
	bool              extended;
	uint32_t          app_timeout_ms;
	uint64_t          last_feed_ms; /* guarded by lock */
	struct k_spinlock lock;
	struct k_timer    keeper;
};

/* Indexed by the TOPS[1:0] encoding (wdt_rzv_timeout.h). */
static const wdt_timeout_t wdt_rzv_timeouts[] = { WDT_TIMEOUT_1024,
	                                              WDT_TIMEOUT_4096,
	                                              WDT_TIMEOUT_8192,
	                                              WDT_TIMEOUT_16384 };

BUILD_ASSERT(ARRAY_SIZE(wdt_rzv_timeouts) == ARRAY_SIZE(wdt_rzv_tops_counts));
/* The FSP enum values ARE the CKS[3:0] encodings (manual Table 5.4-3); the picker returns raw CKS. */
BUILD_ASSERT(WDT_CLOCK_DIVISION_1 == 0x0 && WDT_CLOCK_DIVISION_16 == 0x2 &&
             WDT_CLOCK_DIVISION_32 == 0x3 && WDT_CLOCK_DIVISION_64 == 0x4 &&
             WDT_CLOCK_DIVISION_128 == 0xF && WDT_CLOCK_DIVISION_256 == 0x5);

static int wdt_rzv_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *config)
{
	const struct wdt_rzv_config *cfg  = dev->config;
	struct wdt_rzv_data         *data = dev->data;
	uint8_t                      tops;
	uint8_t                      cks;

	if (atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EBUSY;
	}
	if (config->window.max == 0 || config->window.min != 0) {
		return -EINVAL;
	}
	if (config->callback != NULL || (config->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
		LOG_ERR("interrupt-only expiry unsupported: no WDT0 NVIC line known for the CM33");
		return -ENOTSUP;
	}
	if ((config->flags & WDT_FLAG_RESET_MASK) != WDT_FLAG_RESET_SOC) {
		LOG_ERR("only a whole-SoC reset is supported");
		return -ENOTSUP;
	}
	if (atomic_test_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET)) {
		return -ENOMEM; /* a single channel */
	}
	const bool direct = wdt_rzv_is_direct(cfg->clock_freq, config->window.max);

	/* Above the hardware ceiling: a short fixed hardware period + a software keeper (see header). */
	if (!wdt_rzv_pick(
	        cfg->clock_freq, direct ? config->window.max : WDT_RZV_EXT_HW_MAX_MS, &tops, &cks)) {
		LOG_ERR("timeout %u ms is shorter than the shortest WDT0 period", config->window.max);
		return -EINVAL;
	}

	data->extended           = !direct;
	data->app_timeout_ms     = config->window.max;
	data->cfg.timeout        = wdt_rzv_timeouts[tops];
	data->cfg.clock_division = (wdt_clock_division_t)cks;
	data->cfg.window_start   = WDT_WINDOW_START_100;
	data->cfg.window_end     = WDT_WINDOW_END_0;
	/* RSTIRQS = 0 on this LSI; the reset comes from CPG_ERRORRST_SEL2 (manual 5.4.2.2.4). */
	data->cfg.reset_control = WDT_RESET_CONTROL_NMI;
	atomic_set_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET);

	return 0;
}

/* System-timer ISR context.  Refreshes only while the app deadline has not passed; once it has, it
 * stops and the hardware expires within one hardware period.  The sole refresher in extended mode
 * (a refresh is a two-write sequence, so the app's feed must never interleave with it).
 */
static void wdt_rzv_keeper(struct k_timer *timer)
{
	struct wdt_rzv_data *data = CONTAINER_OF(timer, struct wdt_rzv_data, keeper);
	bool                 refresh;

	K_SPINLOCK(&data->lock)
	{
		refresh = wdt_rzv_keeper_refresh(
		    (uint64_t)k_uptime_get(), data->last_feed_ms, data->app_timeout_ms);
	}
	if (refresh) {
		(void)g_wdt_on_wdt.refresh(&data->ctrl);
	}
}

static int wdt_rzv_arm(struct wdt_rzv_data *data, uint8_t options)
{
	if ((options & WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		return -ENOTSUP;
	}

	data->cfg.stop_control = (options & WDT_OPT_PAUSE_IN_SLEEP) != 0 ? WDT_STOP_CONTROL_ENABLE
	                                                                 : WDT_STOP_CONTROL_DISABLE;

	if (g_wdt_on_wdt.open(&data->ctrl, &data->cfg) != FSP_SUCCESS) {
		return -EIO;
	}

	/* Route the WDT0 error to a whole-SoC reset (CPG_ERRORRST_SEL2), then start the counter. */
	R_BSP_WDT_SYSTEM_RESET_ENABLE(0, 1);
	if (g_wdt_on_wdt.refresh(&data->ctrl) != FSP_SUCCESS) {
		return -EIO;
	}

	if (data->extended) {
		data->last_feed_ms = (uint64_t)k_uptime_get();
		k_timer_init(&data->keeper, wdt_rzv_keeper, NULL);
		k_timer_start(&data->keeper, K_NO_WAIT, K_MSEC(WDT_RZV_EXT_KEEPER_MS));
	}

	return 0;
}

static int wdt_rzv_setup(const struct device *dev, uint8_t options)
{
	struct wdt_rzv_data *data = dev->data;
	int                  ret;

	if (atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EBUSY;
	}
	if (!atomic_test_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET)) {
		return -EINVAL;
	}

	ret = wdt_rzv_arm(data, options);
	if (ret != 0) {
		/* A rejected setup must free the single channel, or every later
		 * install_timeout() would answer -ENOMEM for good.
		 */
		atomic_clear_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET);
		return ret;
	}

	atomic_set_bit(&data->state, WDT_RZV_ATOMIC_ENABLE);

	return 0;
}

static int wdt_rzv_disable(const struct device *dev)
{
	struct wdt_rzv_data *data = dev->data;

	if (!atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EFAULT;
	}

	LOG_ERR("WDT0 cannot be stopped once started unless the SoC gets a reset");
	return -EPERM; /* the extended-mode keeper therefore never needs a stop path */
}

static int wdt_rzv_feed(const struct device *dev, int channel_id)
{
	struct wdt_rzv_data *data = dev->data;

	if (channel_id != 0 || !atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EINVAL;
	}

	if (data->extended) {
		K_SPINLOCK(&data->lock)
		{
			data->last_feed_ms = (uint64_t)k_uptime_get();
		}
		return 0;
	}

	return g_wdt_on_wdt.refresh(&data->ctrl) == FSP_SUCCESS ? 0 : -EIO;
}

static DEVICE_API(wdt, wdt_rzv_api) = {
	.setup           = wdt_rzv_setup,
	.disable         = wdt_rzv_disable,
	.install_timeout = wdt_rzv_install_timeout,
	.feed            = wdt_rzv_feed,
};

#define WDT_RZV_INIT(inst) \
	BUILD_ASSERT(DT_INST_REG_ADDR(inst) == R_WDT0_BASE, \
	             "renesas,rzv-wdt: the glue drives R_WDT0 (the CM33 WDT) only"); \
	static struct wdt_rzv_data         wdt_rzv_data_##inst; \
	static const struct wdt_rzv_config wdt_rzv_config_##inst = { \
		.clock_freq = DT_INST_PROP(inst, clock_freq), \
	}; \
	DEVICE_DT_INST_DEFINE(inst, \
	                      NULL, \
	                      NULL, \
	                      &wdt_rzv_data_##inst, \
	                      &wdt_rzv_config_##inst, \
	                      POST_KERNEL, \
	                      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
	                      &wdt_rzv_api);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1,
             "renesas,rzv-wdt: one instance only (the vendored r_wdt is fixed to R_WDT0)");

DT_INST_FOREACH_STATUS_OKAY(WDT_RZV_INIT)
