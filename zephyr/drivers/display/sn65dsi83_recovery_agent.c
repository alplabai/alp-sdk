/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADR-0017-ADJACENT, BENCH-UNVERIFIED (see display_sn65dsi83.c).
 *
 * SN65DSI83 recovery on the core that OWNS the I2C bus when another core configured
 * the bridge (compatible "alp,sn65dsi83-recovery"; the problem and the split are in
 * sn65dsi83_recovery.h).  The display core published the CSR table in the shared
 * recipe and no longer touches the bus; this core runs the health pass from a delayable
 * work item on its own I2C driver, so it is serialised with the camera's (or whatever
 * else shares the controller) transfers by that driver's bus lock -- one core, one
 * driver, no cross-core race.
 *
 * Silent until the recipe's magic is set: an image whose display is not behind a bridge
 * never publishes one.  Writes only after the bridge's ID registers read back
 * (sn65dsi83_health_poll()), so a stale recipe from a previous boot with another panel
 * cannot write a CSR bank into anything else on the bus.  The console is printk: the
 * images this runs in turn the logging subsystem off.
 */

#define DT_DRV_COMPAT alp_sn65dsi83_recovery

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>

#include "sn65dsi83_recovery.h"

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(alp_sn65dsi83_recovery) <= 1,
             "sn65dsi83_recovery_agent.c assumes a single alp,sn65dsi83-recovery instance");

struct sn65dsi83_agent_config {
	struct i2c_dt_spec       i2c;
	struct sn65dsi83_recipe *recipe;
};

struct sn65dsi83_agent_data {
	struct k_work_delayable health_work;
	struct sn65dsi83_stats  stats;
};

static void sn65dsi83_agent_work(struct k_work *work)
{
	struct k_work_delayable     *dwork = k_work_delayable_from_work(work);
	struct sn65dsi83_agent_data *data =
	    CONTAINER_OF(dwork, struct sn65dsi83_agent_data, health_work);
	const struct sn65dsi83_agent_config *config = DEVICE_DT_INST_GET(0)->config;
	struct sn65dsi83_recipe             *r      = config->recipe;
	struct sn65dsi83_csr                 csr[SN65_RECIPE_MAX];
	struct sn65dsi83_report              rep;
	uint8_t                              n;

	if (r->magic == SN65_RECIPE_MAGIC) {
		barrier_dmem_fence_full();
		n = r->n;
		if (n > 0U && n <= SN65_RECIPE_MAX) {
			for (uint8_t i = 0; i < n; i++) {
				csr[i] = r->csr[i];
			}
			if (sn65dsi83_health_poll(&config->i2c, csr, n, &data->stats, &rep) == 0 &&
			    rep.health != SN65_HEALTH_OK) {
				printk("sn65dsi83: %s (0x0D=0x%02x 0x0A=0x%02x 0xE5=0x%02x) err=%d "
				       "recoveries=%u failures=%u cleared=%u\n",
				       rep.suppressed                           ? "down, re-init rate-limited"
				       : rep.health == SN65_HEALTH_CLEAR_ERRORS ? "link errors cleared"
				                                                : "lost config, re-init",
				       rep.pll_en,
				       rep.clk_src,
				       rep.err_stat,
				       rep.err,
				       data->stats.recoveries,
				       data->stats.failures,
				       data->stats.errors_cleared);
			}
			/* The display core reads these through sn65dsi83_recovery_count(). */
			r->recoveries     = data->stats.recoveries;
			r->failures       = data->stats.failures;
			r->errors_cleared = data->stats.errors_cleared;
		}
	}

	k_work_reschedule(dwork, K_MSEC(CONFIG_DISPLAY_SN65DSI83_RECOVERY_INTERVAL_MS));
}

static int sn65dsi83_agent_init(const struct device *dev)
{
	struct sn65dsi83_agent_data *data = dev->data;

	/* Late enough that the display core published its recipe before it released the bus
	 * (the bus is ours only after that), and the camera has had its first transfers. */
	k_work_init_delayable(&data->health_work, sn65dsi83_agent_work);
	k_work_schedule(&data->health_work, K_SECONDS(5));
	return 0;
}

static struct sn65dsi83_agent_data         sn65dsi83_agent_data_0;
static const struct sn65dsi83_agent_config sn65dsi83_agent_config_0 = {
	.i2c    = I2C_DT_SPEC_INST_GET(0),
	.recipe = (struct sn65dsi83_recipe *)DT_INST_PROP(0, recipe_address),
};

DEVICE_DT_INST_DEFINE(0,
                      sn65dsi83_agent_init,
                      NULL,
                      &sn65dsi83_agent_data_0,
                      &sn65dsi83_agent_config_0,
                      POST_KERNEL,
                      CONFIG_APPLICATION_INIT_PRIORITY,
                      NULL);
