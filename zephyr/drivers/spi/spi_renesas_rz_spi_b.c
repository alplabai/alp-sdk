/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas RZ SPI_B (RSPI) controller driver.
 *
 * The new-generation RZ MPUs (RZ/V2N R9A09G056, RZ/V2H R9A09G057, ...) replace
 * the classic RSPI with the SPI_B IP, served by the FSP `r_spi_b` module.  The
 * upstream `renesas,rz-spi` driver targets the classic `r_spi` module (RZ/T2,
 * RZ/N2) which the new-gen `rzv` FSP family does not ship -- hence this
 * dedicated `renesas,rz-spi-b` driver.  It is structurally the RA8 SPI_B driver
 * (drivers/spi/spi_b_renesas_ra8.c, same FSP module + identical R_SPI_B0
 * registers) with the RZ specifics:
 *
 *   - the communication clock comes from the `clk-src` devicetree property
 *     (0 = SCISPICLK, 1 = PCLK) instead of an RA clock-control phandle;
 *   - rxi/txi are INTC *selectable* interrupts: on the CM33 they reach the NVIC
 *     only once the INTC INTSEL mux routes the SPI source to the SELECT slot the
 *     devicetree assigns as the rxi/txi IRQ (SPIk_RXI = 77 + 2*k,
 *     SPIk_TXI = 78 + 2*k per bsp_irq_id.h).  tei/eri are fixed CM33 vectors.
 *     (RA links events through R_ICU->IELSR instead.)
 */

#define DT_DRV_COMPAT renesas_rz_spi_b

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include "r_spi_b.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rz_spi_b);

#include "spi_context.h"

#if defined(CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT)
void spi_b_rxi_isr(void);
void spi_b_txi_isr(void);
void spi_b_tei_isr(void);
void spi_b_eri_isr(void);

/*
 * INTSEL routing for the selectable rxi/txi interrupts (see file header).  The
 * CM33 INTSEL window starts at intc + 0x200; each 32-bit register packs three
 * 10-bit SELECT-slot fields, indexed by (slot - SEL0_BASE) where SEL0 = 353.
 * Mirrors drivers/i2c/i2c_renesas_rz_riic.c.
 */
#define RZ_SPI_B_INTSEL_BASE     (DT_REG_ADDR(DT_NODELABEL(intc)) + 0x200)
#define RZ_SPI_B_INTSEL_OFF(y)   ((y) - 353)
#define RZ_SPI_B_INTSEL_RD(y)    sys_read32(RZ_SPI_B_INTSEL_BASE + (RZ_SPI_B_INTSEL_OFF(y) / 3) * 4)
#define RZ_SPI_B_INTSEL_WR(y, v) sys_write32((v), RZ_SPI_B_INTSEL_BASE + (RZ_SPI_B_INTSEL_OFF(y) / 3) * 4)
#define RZ_SPI_B_INTSEL_MASK(y)  (BIT_MASK(10) << ((RZ_SPI_B_INTSEL_OFF(y) % 3) * 10))

static inline void rz_spi_b_intsel_connect(uint32_t slot_irq, uint32_t spi_event)
{
	uint32_t v = RZ_SPI_B_INTSEL_RD(slot_irq);

	v &= ~RZ_SPI_B_INTSEL_MASK(slot_irq);
	v |= FIELD_PREP(RZ_SPI_B_INTSEL_MASK(slot_irq), spi_event);
	RZ_SPI_B_INTSEL_WR(slot_irq, v);
}
#endif /* CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT */

struct rz_spi_b_config {
	const struct pinctrl_dev_config *pcfg;
	uint8_t clk_src;
};

struct rz_spi_b_data {
	struct spi_context ctx;
	uint8_t dfs;
	struct st_spi_b_instance_ctrl spi;
	struct st_spi_cfg fsp_config;
	struct st_spi_b_extended_cfg fsp_config_extend;
#if defined(CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT)
	uint32_t data_len;
#endif
};

static void spi_cb(spi_callback_args_t *p_args)
{
	struct device *dev = (struct device *)p_args->p_context;
	struct rz_spi_b_data *data = dev->data;

	switch (p_args->event) {
	case SPI_EVENT_TRANSFER_COMPLETE:
		spi_context_cs_control(&data->ctx, false);
		spi_context_complete(&data->ctx, dev, 0);
		break;
	case SPI_EVENT_ERR_MODE_FAULT:    /* Mode fault error */
	case SPI_EVENT_ERR_READ_OVERFLOW: /* Read overflow error */
	case SPI_EVENT_ERR_PARITY:        /* Parity error */
	case SPI_EVENT_ERR_OVERRUN:       /* Overrun error */
	case SPI_EVENT_ERR_FRAMING:       /* Framing error */
	case SPI_EVENT_ERR_MODE_UNDERRUN: /* Underrun error */
		spi_context_cs_control(&data->ctx, false);
		spi_context_complete(&data->ctx, dev, -EIO);
		break;
	default:
		break;
	}
}

static int rz_spi_b_configure(const struct device *dev, const struct spi_config *config)
{
	struct rz_spi_b_data *data = dev->data;
	fsp_err_t fsp_err;
	uint8_t word_size = SPI_WORD_SIZE_GET(config->operation);

	if (spi_context_configured(&data->ctx, config)) {
		/* Nothing to do */
		return 0;
	}

	if (data->spi.open != 0) {
		R_SPI_B_Close(&data->spi);
	}

	if ((config->operation & SPI_FRAME_FORMAT_TI) == SPI_FRAME_FORMAT_TI) {
		return -ENOTSUP;
	}

	if (word_size < 4 || word_size > 32) {
		LOG_ERR("Unsupported SPI word size: %u", word_size);
		return -ENOTSUP;
	}

	if (config->operation & SPI_OP_MODE_SLAVE) {
		data->fsp_config.operating_mode = SPI_MODE_SLAVE;
	} else {
		data->fsp_config.operating_mode = SPI_MODE_MASTER;
	}

	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPOL) {
		data->fsp_config.clk_polarity = SPI_CLK_POLARITY_HIGH;
	} else {
		data->fsp_config.clk_polarity = SPI_CLK_POLARITY_LOW;
	}

	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPHA) {
		data->fsp_config.clk_phase = SPI_CLK_PHASE_EDGE_EVEN;
	} else {
		data->fsp_config.clk_phase = SPI_CLK_PHASE_EDGE_ODD;
	}

	if (config->operation & SPI_TRANSFER_LSB) {
		data->fsp_config.bit_order = SPI_BIT_ORDER_LSB_FIRST;
	} else {
		data->fsp_config.bit_order = SPI_BIT_ORDER_MSB_FIRST;
	}

	if (config->frequency > 0) {
		fsp_err = R_SPI_B_CalculateBitrate(config->frequency,
						   data->fsp_config_extend.clock_source,
						   &data->fsp_config_extend.spck_div);
		__ASSERT(fsp_err == 0, "spi_b: spi frequency calculate error: %d", fsp_err);
	}

	data->fsp_config_extend.spi_comm = SPI_B_COMMUNICATION_FULL_DUPLEX;
	if (spi_cs_is_gpio(config) || !IS_ENABLED(CONFIG_SPI_RENESAS_RZ_SPI_B_USE_HW_SS)) {
		data->fsp_config_extend.spi_clksyn = SPI_B_SSL_MODE_CLK_SYN;
	} else {
		data->fsp_config_extend.spi_clksyn = SPI_B_SSL_MODE_SPI;
		data->fsp_config_extend.ssl_select = SPI_B_SSL_SELECT_SSL0;
	}

	data->fsp_config.p_extend = &data->fsp_config_extend;

	data->fsp_config.p_callback = spi_cb;
	data->fsp_config.p_context = (void *)dev;
	fsp_err = R_SPI_B_Open(&data->spi, &data->fsp_config);
	if (fsp_err != FSP_SUCCESS) {
		LOG_ERR("R_SPI_B_Open error: %d", fsp_err);
		return -EINVAL;
	}
	data->ctx.config = config;

	return 0;
}

static bool rz_spi_b_transfer_ongoing(struct rz_spi_b_data *data)
{
	return (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx));
}

#ifndef CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT
static int rz_spi_b_transceive_slave(struct rz_spi_b_data *data)
{
	R_SPI_B0_Type *p_spi_reg = data->spi.p_regs;

	if (p_spi_reg->SPSR_b.SPTEF && spi_context_tx_on(&data->ctx)) {
		uint32_t tx;

		if (data->ctx.tx_buf != NULL) {
			if (data->dfs > 2) {
				tx = *(uint32_t *)(data->ctx.tx_buf);
			} else if (data->dfs > 1) {
				tx = *(uint16_t *)(data->ctx.tx_buf);
			} else {
				tx = *(uint8_t *)(data->ctx.tx_buf);
			}
		} else {
			tx = 0;
		}
		/* Clear Transmit Empty flag */
		p_spi_reg->SPSRC = R_SPI_B0_SPSRC_SPTEFC_Msk;

		p_spi_reg->SPDR = tx;

		spi_context_update_tx(&data->ctx, data->dfs, 1);
	} else {
		p_spi_reg->SPCR_b.SPTIE = 0;
	}

	if (p_spi_reg->SPSR_b.SPRF && spi_context_rx_buf_on(&data->ctx)) {
		uint32_t rx;

		rx = p_spi_reg->SPDR;
		/* Clear Receive Full flag */
		p_spi_reg->SPSRC = R_SPI_B0_SPSRC_SPRFC_Msk;
		if (data->dfs > 2) {
			UNALIGNED_PUT(rx, (uint32_t *)data->ctx.rx_buf);
		} else if (data->dfs > 1) {
			UNALIGNED_PUT(rx, (uint16_t *)data->ctx.rx_buf);
		} else {
			UNALIGNED_PUT(rx, (uint8_t *)data->ctx.rx_buf);
		}
		spi_context_update_rx(&data->ctx, data->dfs, 1);
	}

	return 0;
}

static int rz_spi_b_transceive_master(struct rz_spi_b_data *data)
{
	R_SPI_B0_Type *p_spi_reg = data->spi.p_regs;
	uint32_t tx;
	uint32_t rx;

	/* Tx transfer */
	if (spi_context_tx_buf_on(&data->ctx)) {
		if (data->dfs > 2) {
			tx = *(uint32_t *)(data->ctx.tx_buf);
		} else if (data->dfs > 1) {
			tx = *(uint16_t *)(data->ctx.tx_buf);
		} else {
			tx = *(uint8_t *)(data->ctx.tx_buf);
		}
	} else {
		tx = 0U;
	}

	while (!p_spi_reg->SPSR_b.SPTEF) {
	}
	p_spi_reg->SPDR = tx;

	/* Clear Transmit Empty flag */
	p_spi_reg->SPSRC = R_SPI_B0_SPSRC_SPTEFC_Msk;
	spi_context_update_tx(&data->ctx, data->dfs, 1);

	if (p_spi_reg->SPCR_b.TXMD == 0x0) {
		while (!p_spi_reg->SPSR_b.SPRF) {
		}
		rx = p_spi_reg->SPDR;
		/* Clear Receive Full flag */
		p_spi_reg->SPSRC = R_SPI_B0_SPSRC_SPRFC_Msk;

		/* Rx receive */
		if (spi_context_rx_buf_on(&data->ctx)) {
			if (data->dfs > 2) {
				UNALIGNED_PUT(rx, (uint32_t *)data->ctx.rx_buf);
			} else if (data->dfs > 1) {
				UNALIGNED_PUT(rx, (uint16_t *)data->ctx.rx_buf);
			} else {
				UNALIGNED_PUT(rx, (uint8_t *)data->ctx.rx_buf);
			}
		}
		spi_context_update_rx(&data->ctx, data->dfs, 1);
	}

	return 0;
}

static int rz_spi_b_transceive_data(struct rz_spi_b_data *data)
{
	uint16_t operation = data->ctx.config->operation;

	if (SPI_OP_MODE_GET(operation) == SPI_OP_MODE_MASTER) {
		rz_spi_b_transceive_master(data);
	} else {
		rz_spi_b_transceive_slave(data);
	}

	return 0;
}
#endif /* !CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT */

static int transceive(const struct device *dev, const struct spi_config *config,
		      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
		      bool asynchronous, spi_callback_t cb, void *userdata)
{
	struct rz_spi_b_data *data = dev->data;
	int ret = 0;

	if (!tx_bufs && !rx_bufs) {
		return 0;
	}

#ifndef CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT
	if (asynchronous) {
		return -ENOTSUP;
	}
#endif

	spi_context_lock(&data->ctx, asynchronous, cb, userdata, config);

	ret = rz_spi_b_configure(dev, config);
	if (ret) {
		goto end;
	}
	data->dfs = ((SPI_WORD_SIZE_GET(config->operation) - 1) / 8) + 1;
	/* Set buffers info */
	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, data->dfs);

	spi_context_cs_control(&data->ctx, true);

	if ((!spi_context_tx_buf_on(&data->ctx)) && (!spi_context_rx_buf_on(&data->ctx))) {
		/* If current buffer has no data, do nothing */
		goto end;
	}

#ifdef CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT
	spi_bit_width_t spi_width =
		(spi_bit_width_t)(SPI_WORD_SIZE_GET(data->ctx.config->operation) - 1);

	if (data->ctx.rx_len == 0) {
		data->data_len = spi_context_is_slave(&data->ctx)
					 ? spi_context_total_tx_len(&data->ctx)
					 : data->ctx.tx_len;
	} else if (data->ctx.tx_len == 0) {
		data->data_len = spi_context_is_slave(&data->ctx)
					 ? spi_context_total_rx_len(&data->ctx)
					 : data->ctx.rx_len;
	} else {
		data->data_len = spi_context_is_slave(&data->ctx)
					 ? MAX(spi_context_total_tx_len(&data->ctx),
					       spi_context_total_rx_len(&data->ctx))
					 : MIN(data->ctx.tx_len, data->ctx.rx_len);
	}

	if (data->ctx.rx_buf == NULL) {
		R_SPI_B_Write(&data->spi, data->ctx.tx_buf, data->data_len, spi_width);
	} else if (data->ctx.tx_buf == NULL) {
		R_SPI_B_Read(&data->spi, data->ctx.rx_buf, data->data_len, spi_width);
	} else {
		R_SPI_B_WriteRead(&data->spi, data->ctx.tx_buf, data->ctx.rx_buf, data->data_len,
				  spi_width);
	}
	ret = spi_context_wait_for_completion(&data->ctx);

#else
	R_SPI_B0_Type *p_spi_reg = data->spi.p_regs;

	p_spi_reg->SPCR_b.TXMD = 0x0; /* tx - rx */
	if (!spi_context_rx_on(&data->ctx)) {
		p_spi_reg->SPCR_b.TXMD = 0x1; /* tx only */
	}

	/* Clear FIFOs */
	p_spi_reg->SPFCR = 1;

	/* Enable the SPI Transfer. */
	p_spi_reg->SPCR_b.SPE = 1;
	p_spi_reg->SPCMD0 |= (uint32_t)(SPI_WORD_SIZE_GET(data->ctx.config->operation) - 1)
			     << R_SPI_B0_SPCMD0_SPB_Pos;
	do {
		rz_spi_b_transceive_data(data);
	} while (rz_spi_b_transfer_ongoing(data));

	/* Wait for transmission complete */
	while (p_spi_reg->SPSR_b.IDLNF) {
	}

	/* Disable the SPI Transfer. */
	p_spi_reg->SPCR_b.SPE = 0;

	spi_context_cs_control(&data->ctx, false);
#endif
#ifdef CONFIG_SPI_SLAVE
	if (spi_context_is_slave(&data->ctx) && !ret) {
		ret = data->ctx.recv_frames;
	}
#endif /* CONFIG_SPI_SLAVE */

end:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int rz_spi_b_transceive(const struct device *dev, const struct spi_config *config,
			       const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs)
{
	return transceive(dev, config, tx_bufs, rx_bufs, false, NULL, NULL);
}

#ifdef CONFIG_SPI_ASYNC
static int rz_spi_b_transceive_async(const struct device *dev, const struct spi_config *config,
				     const struct spi_buf_set *tx_bufs,
				     const struct spi_buf_set *rx_bufs, spi_callback_t cb,
				     void *userdata)
{
	return transceive(dev, config, tx_bufs, rx_bufs, true, cb, userdata);
}
#endif /* CONFIG_SPI_ASYNC */

static int rz_spi_b_release(const struct device *dev, const struct spi_config *config)
{
	struct rz_spi_b_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, rz_spi_b_driver_api) = {
	.transceive = rz_spi_b_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = rz_spi_b_transceive_async,
#endif /* CONFIG_SPI_ASYNC */
	.release = rz_spi_b_release,
};

static int rz_spi_b_init(const struct device *dev)
{
	const struct rz_spi_b_config *config = dev->config;
	struct rz_spi_b_data *data = dev->data;
	int ret;

	/* Communication clock source (0 = SCISPICLK, 1 = PCLK) from devicetree. */
	data->fsp_config_extend.clock_source = (spi_b_clock_source_t)config->clk_src;

	/* Configure dt provided device signals when available */
	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#if defined(CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT)

static void rz_spi_retransmit(struct rz_spi_b_data *data)
{
	spi_bit_width_t spi_width =
		(spi_bit_width_t)(SPI_WORD_SIZE_GET(data->ctx.config->operation) - 1);

	if (data->ctx.rx_len == 0) {
		data->data_len = data->ctx.tx_len;
		data->spi.p_tx_data = data->ctx.tx_buf;
		data->spi.p_rx_data = NULL;
	} else if (data->ctx.tx_len == 0) {
		data->data_len = data->ctx.rx_len;
		data->spi.p_tx_data = NULL;
		data->spi.p_rx_data = data->ctx.rx_buf;
	} else {
		data->data_len = MIN(data->ctx.tx_len, data->ctx.rx_len);
		data->spi.p_tx_data = data->ctx.tx_buf;
		data->spi.p_rx_data = data->ctx.rx_buf;
	}

	data->spi.bit_width = spi_width;
	data->spi.rx_count = 0;
	data->spi.tx_count = 0;
	data->spi.count = data->data_len;

	data->spi.p_regs->SPSRC = R_SPI_B0_SPSRC_SPTEFC_Msk;
}

static void rz_spi_rxi_isr(const struct device *dev)
{
#ifndef CONFIG_SPI_SLAVE
	ARG_UNUSED(dev);
	spi_b_rxi_isr();
#else
	struct rz_spi_b_data *data = dev->data;

	spi_b_rxi_isr();
	if (spi_context_is_slave(&data->ctx) && data->spi.rx_count == data->spi.count) {

		if (data->ctx.rx_buf != NULL && data->ctx.tx_buf != NULL) {
			data->ctx.recv_frames = MIN(spi_context_total_tx_len(&data->ctx),
						    spi_context_total_rx_len(&data->ctx));
		} else if (data->ctx.tx_buf == NULL) {
			data->ctx.recv_frames = data->data_len;
		} else {
			/* Do nothing */
		}

		R_BSP_IrqDisable(data->fsp_config.tei_irq);

		/* Writing 0 to SPE generates a TXI IRQ.  Disable the TXI IRQ. */
		R_BSP_IrqDisable(data->fsp_config.txi_irq);

		/* Disable the SPI Transfer. */
		data->spi.p_regs->SPCR_b.SPE = 0;

		/* Re-enable the TXI IRQ and clear the pending IRQ. */
		R_BSP_IrqEnable(data->fsp_config.txi_irq);

		spi_context_cs_control(&data->ctx, false);
		spi_context_complete(&data->ctx, dev, 0);
	}
#endif
}

static void rz_spi_txi_isr(const struct device *dev)
{
	ARG_UNUSED(dev);
	spi_b_txi_isr();
}

static void rz_spi_tei_isr(const struct device *dev)
{
	struct rz_spi_b_data *data = dev->data;

	if (data->spi.rx_count == data->spi.count) {
		spi_context_update_rx(&data->ctx, 1, data->data_len);
	}
	if (data->spi.tx_count == data->spi.count) {
		spi_context_update_tx(&data->ctx, 1, data->data_len);
	}
	if (rz_spi_b_transfer_ongoing(data)) {
		/* Clear the pending (fixed CM33) tei vector before re-arming. */
		NVIC_ClearPendingIRQ((IRQn_Type)data->fsp_config.tei_irq);
		rz_spi_retransmit(data);
	} else {
		spi_b_tei_isr();
	}
}

static void rz_spi_eri_isr(const struct device *dev)
{
	ARG_UNUSED(dev);
	spi_b_eri_isr();
}

#define RZ_SPI_B_IRQ_CONFIG_INIT(index)                                                            \
	do {                                                                                       \
		ARG_UNUSED(dev);                                                                    \
		/* rxi/txi are INTC selectable -> route the SPI source to the SELECT slot. */      \
		rz_spi_b_intsel_connect(DT_INST_IRQ_BY_NAME(index, rxi, irq),                       \
					77 + 2 * DT_INST_PROP(index, channel));                    \
		rz_spi_b_intsel_connect(DT_INST_IRQ_BY_NAME(index, txi, irq),                       \
					78 + 2 * DT_INST_PROP(index, channel));                    \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, rxi, irq),                                   \
			    DT_INST_IRQ_BY_NAME(index, rxi, priority), rz_spi_rxi_isr,              \
			    DEVICE_DT_INST_GET(index), 0);                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, txi, irq),                                   \
			    DT_INST_IRQ_BY_NAME(index, txi, priority), rz_spi_txi_isr,              \
			    DEVICE_DT_INST_GET(index), 0);                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, tei, irq),                                   \
			    DT_INST_IRQ_BY_NAME(index, tei, priority), rz_spi_tei_isr,              \
			    DEVICE_DT_INST_GET(index), 0);                                          \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(index, eri, irq),                                   \
			    DT_INST_IRQ_BY_NAME(index, eri, priority), rz_spi_eri_isr,              \
			    DEVICE_DT_INST_GET(index), 0);                                          \
		irq_enable(DT_INST_IRQ_BY_NAME(index, rxi, irq));                                   \
		irq_enable(DT_INST_IRQ_BY_NAME(index, txi, irq));                                   \
		irq_enable(DT_INST_IRQ_BY_NAME(index, eri, irq));                                   \
	} while (0)

#else

#define RZ_SPI_B_IRQ_CONFIG_INIT(index)

#endif /* CONFIG_SPI_RENESAS_RZ_SPI_B_INTERRUPT */

#define RZ_SPI_B_INIT(index)                                                                       \
	PINCTRL_DT_INST_DEFINE(index);                                                              \
                                                                                                   \
	static const struct rz_spi_b_config rz_spi_b_config_##index = {                             \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(index),                                      \
		.clk_src = DT_INST_PROP(index, clk_src),                                            \
	};                                                                                         \
                                                                                                   \
	static struct rz_spi_b_data rz_spi_b_data_##index = {                                       \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(index), ctx)                            \
			SPI_CONTEXT_INIT_LOCK(rz_spi_b_data_##index, ctx),                         \
		SPI_CONTEXT_INIT_SYNC(rz_spi_b_data_##index, ctx),                                  \
		.fsp_config =                                                                       \
			{                                                                          \
				.channel = DT_INST_PROP(index, channel),                           \
				.rxi_ipl = DT_INST_IRQ_BY_NAME(index, rxi, priority),              \
				.rxi_irq = DT_INST_IRQ_BY_NAME(index, rxi, irq),                   \
				.txi_ipl = DT_INST_IRQ_BY_NAME(index, txi, priority),              \
				.txi_irq = DT_INST_IRQ_BY_NAME(index, txi, irq),                   \
				.tei_ipl = DT_INST_IRQ_BY_NAME(index, tei, priority),              \
				.tei_irq = DT_INST_IRQ_BY_NAME(index, tei, irq),                   \
				.eri_ipl = DT_INST_IRQ_BY_NAME(index, eri, priority),              \
				.eri_irq = DT_INST_IRQ_BY_NAME(index, eri, irq),                   \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static int rz_spi_b_init_##index(const struct device *dev)                                  \
	{                                                                                          \
		int err = rz_spi_b_init(dev);                                                       \
                                                                                                   \
		if (err != 0) {                                                                    \
			return err;                                                                \
		}                                                                                  \
		RZ_SPI_B_IRQ_CONFIG_INIT(index);                                                    \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(index, rz_spi_b_init_##index, NULL, &rz_spi_b_data_##index,           \
			      &rz_spi_b_config_##index, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,      \
			      &rz_spi_b_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RZ_SPI_B_INIT)
