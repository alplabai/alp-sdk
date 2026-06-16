/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N supervisor power-management auxiliary -- see v2n_power_mgmt.h
 * for the contract.
 *
 * Compiled in only when CONFIG_ALP_SDK_V2N_POWER_MGMT=y AND
 * CONFIG_ALP_SDK_V2N_SUPERVISOR=y.  See the !V2N stub at the bottom.
 *
 * Concurrency model:
 *
 *   - The P65 rising-edge IRQ runs in Zephyr's interrupt context.
 *     The handler schedules a workqueue item (g_pwr.work) that does
 *     the actual DA9292 + GPIO work in a thread context where it
 *     can take the supervisor's mutex.  Doing the I²C transaction
 *     directly in the ISR would block the IRQ for ~5 ms and risk
 *     missing edges on other sources.
 *   - The work item acquires BRD_I²C via the supervisor, programs
 *     CH2=0.75 V via da9292_v2n_m1_enable_deepx_rail (which polls
 *     CH2 PG internally), releases the bus, and drives P64 high.
 *   - The DA9292 driver context is owned by this module (g_pwr.dev).
 *     da9292_init() in our SYS_INIT runs with the supervisor's
 *     BRD_I²C lock held so a race with the bridge dispatcher
 *     during the init read cannot land between the I²C
 *     transactions that the init issues.
 */

#include "v2n_power_mgmt.h"

#if defined(CONFIG_ALP_SDK_V2N_POWER_MGMT) && defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/chips/da9292.h"
#include "v2n_supervisor.h"

LOG_MODULE_REGISTER(alp_v2n_power_mgmt, CONFIG_LOG_DEFAULT_LEVEL);

/* DT spec aliases.  The Zephyr device tree for an E1M-V2N101 board
 * is expected to publish:
 *
 *   v2n-deepx-pwr-en-req-gpios   = <&renesas_gpioN PORT_BIT GPIO_ACTIVE_HIGH>;
 *   v2n-deepx-core-0p75-en-gpios = <&renesas_gpioN PORT_BIT GPIO_ACTIVE_HIGH>;
 *
 * If either alias is absent the module compiles to no-ops + the
 * init function returns NOSUPPORT.  Board boards that don't have
 * the DEEPX rail (V2N base SoMs without the M1 DEEPX add-on)
 * legitimately don't populate these aliases. */
#define V2N_PWR_EN_REQ_NODE DT_ALIAS(v2n_deepx_pwr_en_req)
#define V2N_CORE_0P75_NODE DT_ALIAS(v2n_deepx_core_0p75_en)

#if DT_NODE_HAS_STATUS(V2N_PWR_EN_REQ_NODE, okay) && DT_NODE_HAS_STATUS(V2N_CORE_0P75_NODE, okay)

static const struct gpio_dt_spec g_pwr_en_req = GPIO_DT_SPEC_GET(V2N_PWR_EN_REQ_NODE, gpios);
static const struct gpio_dt_spec g_core_0p75  = GPIO_DT_SPEC_GET(V2N_CORE_0P75_NODE, gpios);

static struct {
	da9292_t             dev;
	bool                 initialised;
	struct gpio_callback cb;
	struct k_work        work;
} g_pwr;

/* Workqueue handler -- runs in thread context, can take mutexes
 * + block on I²C.  Drives the full DEEPX rail-up sequence under
 * the supervisor's BRD_I²C lock + then drives P64 high so the
 * downstream consumers see VCORE_0P75 enabled. */
static void v2n_pwr_work_handler(struct k_work *work)
{
	(void)work;
	if (!g_pwr.initialised) return;

	alp_i2c_t   *i2c = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_brd_i2c_acquire(&i2c);
	if (s != ALP_OK) {
		LOG_WRN("BRD_I2C acquire failed (%d); deferring DEEPX bring-up", (int)s);
		return;
	}

	/* The DA9292 ctx already binds the supervisor's I²C handle
     * (set in alp_z_v2n_power_mgmt_init); the call walks the
     * rail-up sequence + polls CH2 PG up to its caller-supplied
     * timeout. */
	s = da9292_v2n_m1_enable_deepx_rail(&g_pwr.dev,
	                                    /* 5 ms timeout per DA9292
                                         * soft-start figures */
	                                    5000u);
	alp_z_v2n_supervisor_brd_i2c_release();

	if (s != ALP_OK) {
		LOG_ERR("DEEPX rail bring-up failed (%d) -- P64 stays low", (int)s);
		return;
	}

	/* Release the host-side gate.  Board wiring routes P64 into
     * the EN2 pin of the DA9292 (belt-and-braces against the
     * register-side enable) + into any downstream consumers
     * gated on the VCORE_0P75 rail being live. */
	const int gpio_rc = gpio_pin_set_dt(&g_core_0p75, 1);
	if (gpio_rc != 0) {
		LOG_ERR("gpio_pin_set_dt(P64=1) failed (%d)", gpio_rc);
		return;
	}
	LOG_INF("DEEPX 0.75 V rail up + P64 driven high");
}

static void v2n_pwr_irq_handler(const struct device *port, struct gpio_callback *cb,
                                gpio_port_pins_t pins)
{
	(void)port;
	(void)cb;
	(void)pins;
	/* Defer to the workqueue -- da9292 transactions take ~5 ms
     * which is too long for an IRQ. */
	k_work_submit(&g_pwr.work);
}

alp_status_t alp_z_v2n_power_mgmt_init(void)
{
	if (g_pwr.initialised) return ALP_OK; /* idempotent */

	/* Both GPIO controllers must be DT-bound before we touch them.
     * A missing controller usually means a board overlay that
     * forgot to wire DEEPX -- legitimate on the non-M1 V2N SKU. */
	if (!gpio_is_ready_dt(&g_pwr_en_req) || !gpio_is_ready_dt(&g_core_0p75)) {
		return ALP_ERR_NOT_READY;
	}

	int rc = gpio_pin_configure_dt(&g_core_0p75, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) return ALP_ERR_IO;

	rc = gpio_pin_configure_dt(&g_pwr_en_req, GPIO_INPUT);
	if (rc != 0) return ALP_ERR_IO;
	rc = gpio_pin_interrupt_configure_dt(&g_pwr_en_req, GPIO_INT_EDGE_RISING);
	if (rc != 0) return ALP_ERR_IO;

	gpio_init_callback(&g_pwr.cb, v2n_pwr_irq_handler, BIT(g_pwr_en_req.pin));
	rc = gpio_add_callback(g_pwr_en_req.port, &g_pwr.cb);
	if (rc != 0) return ALP_ERR_IO;

	k_work_init(&g_pwr.work, v2n_pwr_work_handler);

	/* Run the DA9292 base init under the supervisor's I²C lock so
     * the GD32 bridge dispatcher can't interleave probe transactions
     * with our register reads. */
	alp_i2c_t   *i2c = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_brd_i2c_acquire(&i2c);
	if (s != ALP_OK) {
		gpio_remove_callback(g_pwr_en_req.port, &g_pwr.cb);
		return s;
	}
	s = da9292_init(&g_pwr.dev, i2c, DA9292_I2C_ADDR_V2N);
	if (s == ALP_OK) {
		s = da9292_v2n_base_init(&g_pwr.dev);
	}
	alp_z_v2n_supervisor_brd_i2c_release();
	if (s != ALP_OK) {
		gpio_remove_callback(g_pwr_en_req.port, &g_pwr.cb);
		return s;
	}

	g_pwr.initialised = true;
	LOG_INF("V2N DEEPX rail-mgmt initialised -- waiting on P65 rising edge");
	return ALP_OK;
}

/* Hook into Zephyr SYS_INIT at APPLICATION priority so it runs
 * after the GPIO + I²C controllers but before user code; the
 * supervisor singleton's SYS_INIT runs at POST_KERNEL so
 * alp_z_v2n_supervisor_brd_i2c_acquire is safe to call from here. */
static int v2n_pwr_sys_init(void)
{
	const alp_status_t s = alp_z_v2n_power_mgmt_init();
	if (s != ALP_OK) {
		LOG_WRN("V2N DEEPX rail-mgmt init returned %d -- DEEPX unavailable", (int)s);
	}
	return 0;
}
SYS_INIT(v2n_pwr_sys_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#else /* DT aliases missing -- board doesn't wire DEEPX */

alp_status_t alp_z_v2n_power_mgmt_init(void)
{
	/* DT aliases not populated -- this board doesn't have the
     * DEEPX rail.  Surface NOSUPPORT so a misconfigured board
     * doesn't silently look "fine". */
	return ALP_ERR_NOSUPPORT;
}

#endif /* DT_NODE_HAS_STATUS(...) */

#else /* !CONFIG_ALP_SDK_V2N_POWER_MGMT || !CONFIG_ALP_SDK_V2N_SUPERVISOR */

alp_status_t alp_z_v2n_power_mgmt_init(void)
{
	return ALP_ERR_NOSUPPORT;
}

#endif /* CONFIG_ALP_SDK_V2N_POWER_MGMT && CONFIG_ALP_SDK_V2N_SUPERVISOR */
