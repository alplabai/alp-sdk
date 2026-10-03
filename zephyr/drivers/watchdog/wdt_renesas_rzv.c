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
 * Behaviour:
 *   - reset-only: WDT_FLAG_RESET_SOC.  The expiry request is routed to a whole-SoC reset by
 *     R_BSP_WDT_SYSTEM_RESET_ENABLE(0, 1) (CPG_ERRORRST_SEL2), so an expiry resets the whole SoM, not
 *     just the M33.  Interrupt-only (callback) and WDT_FLAG_RESET_CPU_CORE are -ENOTSUP: hal_renesas
 *     rzv2n names no WDT NVIC line for the CM33, and the hardware has no per-core reset.
 *   - no window: window.min must be 0 (the hardware window is fixed at 0..100 %).
 *   - the achieved timeout is the longest period <= window.max (never longer than asked); the clock
 *     comes from the node's `clock-freq`.
 *   - WDT_OPT_PAUSE_IN_SLEEP maps to WDTCSTPR.SLCSTP; WDT_OPT_PAUSE_HALTED_BY_DBG is -ENOTSUP.
 *   - once started the watchdog cannot be stopped (wdt_disable() -> -EPERM), as on the other RZ WDTs.
 * Not verified on silicon: see docs/hil/rzv-wdt0-cm33.md for the ordered bench steps.
 * ============================================================================
 */

#define DT_DRV_COMPAT renesas_rzv_wdt

#include <errno.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "r_wdt.h"

LOG_MODULE_REGISTER(wdt_renesas_rzv, CONFIG_WDT_LOG_LEVEL);

#define WDT_RZV_ATOMIC_ENABLE      0
#define WDT_RZV_ATOMIC_TIMEOUT_SET 1

struct wdt_rzv_config {
	uint32_t clock_freq;
};

struct wdt_rzv_data {
	wdt_instance_ctrl_t ctrl;
	wdt_cfg_t cfg;
	atomic_t state;
};

/* Every counter length / clock divider pair the WDT0 WDTCR can encode (rzt/rzn layout). */
static const wdt_timeout_t wdt_rzv_timeouts[] = {WDT_TIMEOUT_1024, WDT_TIMEOUT_4096,
						 WDT_TIMEOUT_8192, WDT_TIMEOUT_16384};
static const uint32_t wdt_rzv_cycles[] = {1024, 4096, 8192, 16384};

static const wdt_clock_division_t wdt_rzv_divisions[] = {
	WDT_CLOCK_DIVISION_1,   WDT_CLOCK_DIVISION_4,   WDT_CLOCK_DIVISION_16,
	WDT_CLOCK_DIVISION_32,  WDT_CLOCK_DIVISION_64,  WDT_CLOCK_DIVISION_128,
	WDT_CLOCK_DIVISION_256, WDT_CLOCK_DIVISION_512, WDT_CLOCK_DIVISION_2048,
	WDT_CLOCK_DIVISION_8192};
static const uint32_t wdt_rzv_dividers[] = {1, 4, 16, 32, 64, 128, 256, 512, 2048, 8192};

BUILD_ASSERT(ARRAY_SIZE(wdt_rzv_timeouts) == ARRAY_SIZE(wdt_rzv_cycles));
BUILD_ASSERT(ARRAY_SIZE(wdt_rzv_divisions) == ARRAY_SIZE(wdt_rzv_dividers));

/* Longest period not exceeding max_ms; false when even the shortest is longer. */
static bool wdt_rzv_pick(uint32_t clock_freq, uint32_t max_ms, wdt_timeout_t *timeout,
			 wdt_clock_division_t *division)
{
	uint64_t best_us = 0;

	for (size_t d = 0; d < ARRAY_SIZE(wdt_rzv_divisions); d++) {
		for (size_t t = 0; t < ARRAY_SIZE(wdt_rzv_timeouts); t++) {
			uint64_t us = (uint64_t)wdt_rzv_dividers[d] * wdt_rzv_cycles[t] *
				      1000000ULL / clock_freq;

			if (us <= (uint64_t)max_ms * 1000ULL && us > best_us) {
				best_us = us;
				*timeout = wdt_rzv_timeouts[t];
				*division = wdt_rzv_divisions[d];
			}
		}
	}

	return best_us != 0;
}

static int wdt_rzv_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *config)
{
	const struct wdt_rzv_config *cfg = dev->config;
	struct wdt_rzv_data *data = dev->data;
	wdt_timeout_t timeout;
	wdt_clock_division_t division;

	if (atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EBUSY;
	}
	if (config->window.max == 0 || config->window.min != 0) {
		return -EINVAL;
	}
	if (config->callback != NULL ||
	    (config->flags & WDT_FLAG_RESET_MASK) == WDT_FLAG_RESET_NONE) {
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
	if (!wdt_rzv_pick(cfg->clock_freq, config->window.max, &timeout, &division)) {
		LOG_ERR("timeout %u ms is shorter than the shortest WDT0 period",
			config->window.max);
		return -EINVAL;
	}

	data->cfg.timeout = timeout;
	data->cfg.clock_division = division;
	data->cfg.window_start = WDT_WINDOW_START_100;
	data->cfg.window_end = WDT_WINDOW_END_0;
	data->cfg.reset_control = WDT_RESET_CONTROL_RESET;
	atomic_set_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET);

	return 0;
}

static int wdt_rzv_setup(const struct device *dev, uint8_t options)
{
	struct wdt_rzv_data *data = dev->data;

	if ((options & WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		return -ENOTSUP;
	}
	if (atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EBUSY;
	}
	if (!atomic_test_bit(&data->state, WDT_RZV_ATOMIC_TIMEOUT_SET)) {
		return -EINVAL;
	}

	data->cfg.stop_control = (options & WDT_OPT_PAUSE_IN_SLEEP) != 0
					 ? WDT_STOP_CONTROL_ENABLE
					 : WDT_STOP_CONTROL_DISABLE;

	if (g_wdt_on_wdt.open(&data->ctrl, &data->cfg) != FSP_SUCCESS) {
		return -EIO;
	}

	/* Route the WDT0 error to a whole-SoC reset (CPG_ERRORRST_SEL2), then start the counter. */
	R_BSP_WDT_SYSTEM_RESET_ENABLE(0, 1);
	if (g_wdt_on_wdt.refresh(&data->ctrl) != FSP_SUCCESS) {
		return -EIO;
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
	return -EPERM;
}

static int wdt_rzv_feed(const struct device *dev, int channel_id)
{
	struct wdt_rzv_data *data = dev->data;

	if (channel_id != 0 || !atomic_test_bit(&data->state, WDT_RZV_ATOMIC_ENABLE)) {
		return -EINVAL;
	}

	return g_wdt_on_wdt.refresh(&data->ctrl) == FSP_SUCCESS ? 0 : -EIO;
}

static DEVICE_API(wdt, wdt_rzv_api) = {
	.setup = wdt_rzv_setup,
	.disable = wdt_rzv_disable,
	.install_timeout = wdt_rzv_install_timeout,
	.feed = wdt_rzv_feed,
};

#define WDT_RZV_INIT(inst)                                                                         \
	static struct wdt_rzv_data wdt_rzv_data_##inst;                                            \
	static const struct wdt_rzv_config wdt_rzv_config_##inst = {                               \
		.clock_freq = DT_INST_PROP(inst, clock_freq),                                      \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &wdt_rzv_data_##inst, &wdt_rzv_config_##inst,      \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &wdt_rzv_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_RZV_INIT)
