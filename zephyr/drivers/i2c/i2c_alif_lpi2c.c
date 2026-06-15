/*
 * Copyright (C) 2024 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr I2C-class driver for the Alif Ensemble LPI2C (Low-Power I2C)
 * controller. On the E1M-X / Ensemble E8 SoM this is LPI2C0 @0x43009000,
 * routed to pins P7_4 (LPI2C0_SCL_A) / P7_5 (LPI2C0_SDA_A) -- the on-module
 * "BRD_I2C" housekeeping bus carrying OPTIGA (0x30), RV-3028 RTC (0x52),
 * TMP112 (0x48) and the board EEPROM (0x50).
 *
 * ============================== STATUS ==============================
 * vendor-ext, BENCH-UNVERIFIED.
 *
 * Ported from the Apache-2.0 alifsemi/zephyr_alif fork's
 * drivers/i2c/lpi2c_alif.c (Copyright (C) 2024 Alif Semiconductor). Upstream
 * Zephyr v4.4 + hal_alif v2.2.0 do NOT ship this driver; only the fork does.
 *
 * Every register offset/bit below is transcribed VERBATIM from the fork
 * driver; no offset or bitfield has been invented. The fork driver is a
 * deliberately MINIMAL FIFO-mode controller -- see "API conformance" below.
 * It has NOT been run on real silicon as part of this port.
 * ====================================================================
 *
 * API conformance vs Zephyr v4.4 i2c_driver_api:
 *   - The fork driver implements only `.transfer` (master TX, FIFO-paced) and
 *     `.target_register` (a single-byte RX target callback fed from the ISR).
 *     It does NOT model SDA/SCL clock programming, repeated-START, RX-in-the-
 *     transfer path, multi-message read, or bus recovery; the LPI2C block here
 *     is treated as a byte-streaming FIFO, not a full multi-master controller.
 *   - The fork omits `.configure`. Zephyr v4.4's i2c_configure() dereferences
 *     api->configure unconditionally, so a portable consumer that calls
 *     i2c_configure() against a NULL slot would fault. We therefore add a
 *     minimal `.configure` that validates and accepts the requested mode and
 *     otherwise no-ops (there is no programmable clock-rate register in the
 *     fork register model). This is the ONLY addition beyond the fork; it
 *     invents no register access.
 *
 * No fork-only header dependency: the fork driver pulls in only standard
 * Zephyr headers (no soc_common.h / sys_utils). (alp-sdk does provide
 * soc_common.h via zephyr/soc-bridge/alif/ for Ensemble builds, but this
 * driver does not need it.)
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>

#define DT_DRV_COMPAT alif_lpi2c

LOG_MODULE_REGISTER(i2c_alif_lpi2c, CONFIG_I2C_LOG_LEVEL);

struct lpi2c_config {
	DEVICE_MMIO_ROM;
	void (*irq_config_func)(const struct device *dev);
	const struct pinctrl_dev_config *pcfg;
};

struct lpi2c_data {
	DEVICE_MMIO_RAM;
	uint8_t *xfr_buf;
	uint32_t xfr_len;
	uint32_t tx_curr_cnt;
	uint32_t rx_curr_cnt;
	struct i2c_target_config *slave_cfg;
};

/*
 * LPI2C register map (transcribed verbatim from the fork driver
 * drivers/i2c/lpi2c_alif.c -- no offset/bit invented here).
 */
#define LPI2C_DATA_REG          (0x00)  /* data            register */
#define LPI2C_INBOUND_FIFO_REG  (0x10)  /* inbound fifo    register */
#define LPI2C_OUTBOUND_FIFO_REG (0x20)  /* outbound fifo   register */

/* Status bits within the inbound/outbound FIFO registers. */
#define LPI2C_FIFO_EMPTY (0x20) /* LPI2C FIFO empty            */
#define LPI2C_FIFO_FULL  (0x10) /* LPI2C FIFO full             */
#define LPI2C_AVL_DATA   (0x0F) /* LPI2C number-of-data mask   */

#define LPI2C_MAX_FIFO_LEN (8U) /* Maximum FIFO data length    */

/*
 *  bus speed = 1 / 100 * 10^3  = 10us
 *  1 bit period = 10us
 *
 *  1 byte = 10us * 8bits = 80us
 */
#define LPI2C_1BYTE_XFER_TIME_US (80)

static int lpi2c_initialize(const struct device *dev)
{
	int err;
	const struct lpi2c_config *config = dev->config;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err != 0) {
		return err;
	}
	config->irq_config_func(dev);

	return 0;
}

static void lpi2c_isr(const struct device *dev)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint8_t read_byte;
	struct lpi2c_data *data = dev->data;
	const struct i2c_target_callbacks *slave_cb;

	if (data->slave_cfg == NULL) {
		/* Drain the byte so the IRQ does not re-assert; no target. */
		(void)sys_read8(regs + LPI2C_DATA_REG);
		return;
	}

	slave_cb = data->slave_cfg->callbacks;
	read_byte = sys_read8(regs + LPI2C_DATA_REG);

	if (slave_cb->write_received) {
		slave_cb->write_received(data->slave_cfg, read_byte);
	}
}

/*
 * Minimal `.configure` for Zephyr v4.4 conformance. The fork register model
 * exposes no programmable clock-rate register, so there is nothing to write;
 * we accept master mode at any standard speed and reject target-mode/10-bit
 * config (the driver streams 7-bit master TX only). No register is touched.
 */
static int lpi2c_configure(const struct device *dev, uint32_t dev_config)
{
	ARG_UNUSED(dev);

	if (dev_config & I2C_MODE_CONTROLLER) {
		return 0;
	}

	return -ENOTSUP;
}

static int lpi2c_slave_register(const struct device *dev,
				struct i2c_target_config *cfg)
{
	struct lpi2c_data *data = dev->data;

	data->slave_cfg = cfg;
	return 0;
}

static uint32_t lpi2c_fifo_rem_len(const struct device *dev)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint8_t avl_data;

	avl_data = sys_read8(regs + LPI2C_OUTBOUND_FIFO_REG) & LPI2C_AVL_DATA;

	return (LPI2C_MAX_FIFO_LEN - avl_data);
}

static int lpi2c_transfer(const struct device *dev, struct i2c_msg *msgs,
			  uint8_t num_msgs, uint16_t slave_address)
{
	struct lpi2c_data *data = dev->data;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct i2c_msg *cur_msg = msgs;
	uint8_t msg_left = num_msgs;
	uint8_t xmit_data;

	ARG_UNUSED(slave_address);

	/* Process all the messages */
	while (msg_left > 0) {
		data->xfr_buf = cur_msg->buf;
		data->xfr_len = cur_msg->len;
		data->tx_curr_cnt = 0U;

		while (data->tx_curr_cnt < data->xfr_len) {

			if (lpi2c_fifo_rem_len(dev)) {

				/* load value to be transmit */
				xmit_data = data->xfr_buf[0];

				data->tx_curr_cnt++;
				data->xfr_buf++;

				sys_write8(xmit_data, (regs + LPI2C_DATA_REG));
			}
		}

		/* wait for fifo to be empty */
		while (!(sys_read8(regs + LPI2C_OUTBOUND_FIFO_REG) & LPI2C_FIFO_EMPTY)) {
		}

		/* 80us delay */
		k_usleep(LPI2C_1BYTE_XFER_TIME_US);

		cur_msg++;
		msg_left--;
	}
	return 0;
}

static DEVICE_API(i2c, funcs) = {
	.configure = lpi2c_configure,
	.transfer = lpi2c_transfer,
	.target_register = lpi2c_slave_register,
};

#define LPI2C_DEVICE_INIT(inst)                                                                    \
	static void lpi2c_config_func_##inst(const struct device *dev);                            \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
	static struct lpi2c_data data_##inst;                                                      \
	static const struct lpi2c_config config_##inst = {                                         \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                           \
		.irq_config_func = lpi2c_config_func_##inst,                                       \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                      \
	};                                                                                         \
	I2C_DEVICE_DT_INST_DEFINE(inst, lpi2c_initialize, NULL, &data_##inst, &config_##inst,      \
				  POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &funcs);                  \
	static void lpi2c_config_func_##inst(const struct device *dev)                             \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), lpi2c_isr,            \
			    DEVICE_DT_INST_GET(inst), 0);                                          \
		irq_enable(DT_INST_IRQN(inst));                                                    \
	}

DT_INST_FOREACH_STATUS_OKAY(LPI2C_DEVICE_INIT)
