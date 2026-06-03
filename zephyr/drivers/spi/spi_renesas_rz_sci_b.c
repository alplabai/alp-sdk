/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas RZ SCI-B Simple-SPI controller driver.
 *
 * The on-module GD32G553 supervisor's SPI fast path is wired to RZ/V2N pads
 * P76/P77/P96/P97, which per the PFC (Table 1.2-3) are MOSI7/MISO7/SCK7/SS7 --
 * i.e. SCI channel 7 in clock-synchronous "Simple SPI" mode (sci7@42802800,
 * compatible renesas,rz-sci-b), NOT the dedicated SPI_B IP.  The RZ FSP ships
 * an r_sci_b_uart module but no SCI-SPI module, so the FSP r_sci_b_spi module
 * is vendored from RA into alp-sdk (zephyr/drivers/spi/r_sci_b_spi/) -- the
 * SCI_B register model + the R_BSP_Irq*() and R_BSP_MODULE_START(FSP_IP_SCI) APIs are
 * shared RA<->RZ, so only a small cfg shim is needed (see r_sci_b_spi_cfg.h).
 *
 * This Zephyr glue layer is structurally the RA SCI-B SPI driver
 * (zephyr/drivers/spi/spi_renesas_ra_sci_b.c, same FSP module + identical
 * sci_b_spi_* types) with the RZ specifics:
 *
 *   - DT model: the SPI node is a child of the SoC sci<N> node.  pinctrl,
 *     fixed rxi/txi/tei/eri interrupt vectors and the channel come from the
 *     parent (DT_INST_PARENT), exactly like the RZ SCI-B UART driver
 *     (zephyr/drivers/serial/uart_renesas_rz_sci.c).
 *   - SCI interrupts are FIXED CM33 NVIC vectors (bsp_irq_id.h: SCI7 ERI=156,
 *     RXI=157, TXI=158, TEI=159), so -- unlike the SPI_B IP, whose rxi/txi are
 *     INTC-selectable -- there is no INTSEL routing here; a plain IRQ_CONNECT +
 *     irq_enable (+ the ICU-v2 event link on SoCs that use it) is enough.
 *   - the module communication clock is P5CLK (BSP_FEATURE_SCI_CLOCK); the FSP
 *     handles CPG module-start (R_BSP_MODULE_START(FSP_IP_SCI)) inside Open, so
 *     no clock-control phandle is needed.
 *   - the SCI has no hardware slave-select in master mode (CCR0.SSE is set only
 *     for slaves), so the chip select is a GPIO driven by the SPI context
 *     (the GD32's CS-EXTI needs per-transaction framing).
 */

#define DT_DRV_COMPAT renesas_rz_sci_b_spi

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/interrupt_controller/intc_rz_icu.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/kernel.h>
#include "r_sci_b_spi.h"

/* ── RZ/V2N GD32-bridge chip-select: direct output-latch drive ───────────────
 * On RZ/V2N silicon the Zephyr gpio_rz per-transfer output path (FSP
 * portWrite, used by spi_context_cs_control()) does NOT reach the P97 pad, so
 * the chip-select latch stays high and the GD32's NSS/EXTI8 never sees a
 * per-transaction edge -- it accumulates every byte into one blob that fails
 * CRC and answers STATUS_IO.  Proven on silicon 2026-06-03: with SCK (1 MHz)
 * and MOSI/MISO live, the GD32's spi_rx_len pegs at 69 (buffer full, never
 * reset).  So drive the P97 output latch DIRECTLY via the CM33's secure GPIO
 * alias.  The pin direction (PM9 P97 output: on THIS pad field 0b11/0xC000 =
 * push-pull, set by alp_v2n_pins_assert() below -- field 0b01/0x4000 was swept
 * and drives HIGH-only so CS never pulls low; do NOT "simplify" it to 0x4000)
 * and the SCK7/MOSI/MISO mux are held by the platform against Linux's SD1_CD
 * (P94) clobber; here we only flip the per-transaction output bit.
 * P97 = port 9 pin 7, active-low.  Address = R_GPIO secure base 0x40410020 +
 * P29 (port-9 output-data register, offset 0x09) = 0x40410029.
 * NOTE: board-specific bring-up shim for the gd32 bridge; the durable fix is
 * the gpio_rz/IDAU output path + dropping the Linux SD1_CD pin claim. */
#define ALP_V2N_P9_LATCH 0x40410029u
#define ALP_V2N_CS_MASK  (1u << 7)
/* The GD32 detects CS via PA8->EXTI8: its falling-edge ISR resets the RX
 * staging buffer and preloads the first reply byte, and the rising-edge ISR
 * decodes.  Give those ISRs a setup window after CS-assert (before the first
 * SCK) and a hold window before CS-deassert (so the final byte is latched and
 * the reply is staged) -- without them the first RBNE bytes race the EXTI8 ISR
 * and are dropped/misaligned (seen on silicon as a 2-of-4-byte, A5-84 capture). */
#define ALP_V2N_CS_SETUP_US 60
#define ALP_V2N_CS_HOLD_US  30
static ALWAYS_INLINE void alp_v2n_cs_assert(void)
{
	sys_write8(sys_read8(ALP_V2N_P9_LATCH) & (uint8_t)~ALP_V2N_CS_MASK, ALP_V2N_P9_LATCH);
	k_busy_wait(ALP_V2N_CS_SETUP_US);
}
static ALWAYS_INLINE void alp_v2n_cs_deassert(void)
{
	k_busy_wait(ALP_V2N_CS_HOLD_US);
	sys_write8(sys_read8(ALP_V2N_P9_LATCH) | ALP_V2N_CS_MASK, ALP_V2N_P9_LATCH);
}

/* ── RZ/V2N CM33 pin-ownership: hold P96(SCK7)/P97(CS) against the AMP clobber ──
 * Port 9 is shared across the A55+CM33 AMP split: Linux owns P94 (SD1_CD) and its
 * byte/half-word write to the port-9 PMC9/PM9 registers wipes the CM33's P96
 * (SCK7 peripheral-enable) and P97 (CS output-direction) bits.  The CM33 re-owns
 * them via secure-alias read-modify-write, preserving Linux's P94 bits (PMC9 bit4,
 * PM9 bits[9:8]).  Proven on silicon 2026-06-03 -- identical writes to the bench
 * /dev/mem poke that held the link: PMC9 bit6=1 (P96 peripheral = SCK7); PM9 P97
 * field[15:14]=0b11 (0xC000 = full push-pull output -- field 01/0x4000 drives
 * HIGH-ONLY so CS never pulls low; empirically swept).  PFC9 P96=func2 is applied
 * by pinctrl_apply_state() at init and survives (Linux does a per-nibble RMW on
 * PFC).  Called once at init AND just before each transceive so it wins whenever
 * Linux's clobber lands.  Durable carrier-side fix: drop the Linux SD1_CD (P94)
 * pin claim (meta-alp-sdk .../e1m-x-evk.dtsi); then the init call alone suffices. */
#define ALP_V2N_PWPR        0x40413C04u
#define ALP_V2N_PMC9        0x40410229u
#define ALP_V2N_PM9         0x40410152u
#define ALP_V2N_PWPR_REGWE  (1u << 6)
#define ALP_V2N_PMC9_SCK7   (1u << 6)
#define ALP_V2N_PM9_P97_OUT 0xC000u   /* PM9 bits[15:14]=0b11 = push-pull output */
static ALWAYS_INLINE void alp_v2n_pins_assert(void)
{
	const uint8_t pwpr = sys_read8(ALP_V2N_PWPR);
	sys_write8(pwpr | ALP_V2N_PWPR_REGWE, ALP_V2N_PWPR);
	sys_write8(sys_read8(ALP_V2N_PMC9) | ALP_V2N_PMC9_SCK7, ALP_V2N_PMC9);
	sys_write16((sys_read16(ALP_V2N_PM9) & 0x3FFFu) | ALP_V2N_PM9_P97_OUT, ALP_V2N_PM9);
	sys_write8(pwpr, ALP_V2N_PWPR);
}

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rz_sci_b_spi, CONFIG_SPI_LOG_LEVEL);

#include "spi_context.h"

/* FSP r_sci_b_spi interrupt service routines (vendored module). */
extern void sci_b_spi_rxi_isr(void);
extern void sci_b_spi_txi_isr(void);
extern void sci_b_spi_tei_isr(void);
extern void sci_b_spi_eri_isr(void);

struct rz_sci_b_spi_config {
	const struct pinctrl_dev_config *pcfg;
};

struct rz_sci_b_spi_data {
	struct spi_context ctx;
	sci_b_spi_instance_ctrl_t fsp_ctrl;
	spi_cfg_t fsp_cfg;
	sci_b_spi_extended_cfg_t fsp_ext_cfg;
	uint32_t data_len;
};

static bool rz_sci_b_spi_transfer_ongoing(struct rz_sci_b_spi_data *data)
{
	return (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx));
}

/*
 * Kick the FSP for the next contiguous chunk when the spi_context still has
 * buffers left after a TRANSFER_COMPLETE (the FSP transfers one flat chunk at
 * a time; the Zephyr buffer set may be scattered).  Mirrors the RA driver.
 */
static void rz_sci_b_spi_retransmit(const struct device *dev)
{
	struct rz_sci_b_spi_data *data = dev->data;
	fsp_err_t fsp_err;

	data->data_len = spi_context_max_continuous_chunk(&data->ctx);

	if (data->ctx.rx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Write(&data->fsp_ctrl, data->ctx.tx_buf, data->data_len,
					    SPI_BIT_WIDTH_8_BITS);
	} else if (data->ctx.tx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Read(&data->fsp_ctrl, data->ctx.rx_buf, data->data_len,
					   SPI_BIT_WIDTH_8_BITS);
	} else {
		fsp_err = R_SCI_B_SPI_WriteRead(&data->fsp_ctrl, data->ctx.tx_buf, data->ctx.rx_buf,
						data->data_len, SPI_BIT_WIDTH_8_BITS);
	}

	if (fsp_err != FSP_SUCCESS) {
		LOG_ERR("SCI-SPI continue transfer error: %d", fsp_err);
		spi_context_cs_control(&data->ctx, false);
		alp_v2n_cs_deassert();
		spi_context_complete(&data->ctx, dev, -EIO);
	}
}

static void rz_sci_b_spi_callback(spi_callback_args_t *p_args)
{
	const struct device *dev = (const struct device *)p_args->p_context;
	struct rz_sci_b_spi_data *data = dev->data;
	uint32_t data_receive_len;

	switch (p_args->event) {
	case SPI_EVENT_TRANSFER_COMPLETE:
		if (!spi_context_is_slave(&data->ctx)) {
			if (data->fsp_ctrl.rx_count == data->fsp_ctrl.count ||
			    data->fsp_ctrl.tx_count == data->fsp_ctrl.count) {
				data_receive_len = (!!(data->fsp_ctrl.rx_count))
							   ? (data->fsp_ctrl.rx_count)
							   : data->ctx.rx_len;
				spi_context_update_rx(&data->ctx, 1, data_receive_len);
			}

			if (data->fsp_ctrl.tx_count == data->fsp_ctrl.count) {
				spi_context_update_tx(&data->ctx, 1, data->data_len);
			}

			if (rz_sci_b_spi_transfer_ongoing(data)) {
				rz_sci_b_spi_retransmit(dev);
				return;
			}
		}
#ifdef CONFIG_SPI_SLAVE
		else {
			if (data->fsp_ctrl.rx_count == data->fsp_ctrl.count) {
				if (data->ctx.rx_buf != NULL && data->ctx.tx_buf != NULL) {
					data->ctx.recv_frames =
						MIN(spi_context_total_tx_len(&data->ctx),
						    spi_context_total_rx_len(&data->ctx));
				} else if (data->ctx.tx_buf == NULL) {
					data->ctx.recv_frames = data->data_len;
				}
			}
		}
#endif /* CONFIG_SPI_SLAVE */
		spi_context_cs_control(&data->ctx, false);
		alp_v2n_cs_deassert();
		spi_context_complete(&data->ctx, dev, 0);
		break;
	case SPI_EVENT_ERR_MODE_FAULT:
	case SPI_EVENT_ERR_READ_OVERFLOW:
	case SPI_EVENT_ERR_PARITY:
	case SPI_EVENT_ERR_OVERRUN:
	case SPI_EVENT_ERR_FRAMING:
	case SPI_EVENT_ERR_MODE_UNDERRUN:
		spi_context_cs_control(&data->ctx, false);
		alp_v2n_cs_deassert();
		spi_context_complete(&data->ctx, dev, -EIO);
		break;
	default:
		break;
	}
}

static int rz_sci_b_spi_configure(const struct device *dev, const struct spi_config *config)
{
	struct rz_sci_b_spi_data *data = dev->data;
	fsp_err_t fsp_err;

	if (spi_context_configured(&data->ctx, config)) {
		return 0;
	}

	if (config->operation & SPI_HALF_DUPLEX) {
		LOG_ERR("Half-duplex not supported");
		return -ENOTSUP;
	}

	if ((config->operation & SPI_FRAME_FORMAT_TI) == SPI_FRAME_FORMAT_TI) {
		LOG_ERR("TI frame format not supported");
		return -ENOTSUP;
	}

	if (SPI_MODE_GET(config->operation) & SPI_MODE_LOOP) {
		LOG_ERR("Hardware loopback not supported");
		return -ENOTSUP;
	}

	/* SCI Simple-SPI moves a byte per frame -- 8-bit words only. */
	if (SPI_WORD_SIZE_GET(config->operation) != 8) {
		LOG_ERR("Only 8-bit word size supported");
		return -ENOTSUP;
	}

	if ((config->operation & SPI_OP_MODE_SLAVE) && !IS_ENABLED(CONFIG_SPI_SLAVE)) {
		LOG_ERR("CONFIG_SPI_SLAVE not enabled");
		return -ENOTSUP;
	}

	if (config->operation & SPI_OP_MODE_SLAVE) {
		data->fsp_cfg.operating_mode = SPI_MODE_SLAVE;
	} else {
		if (config->frequency == 0) {
			LOG_ERR("Invalid (zero) frequency");
			return -EINVAL;
		}
		data->fsp_cfg.operating_mode = SPI_MODE_MASTER;
	}

	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPOL) {
		data->fsp_cfg.clk_polarity = SPI_CLK_POLARITY_HIGH;
	} else {
		data->fsp_cfg.clk_polarity = SPI_CLK_POLARITY_LOW;
	}

	if (SPI_MODE_GET(config->operation) & SPI_MODE_CPHA) {
		data->fsp_cfg.clk_phase = SPI_CLK_PHASE_EDGE_EVEN;
	} else {
		data->fsp_cfg.clk_phase = SPI_CLK_PHASE_EDGE_ODD;
	}

	if (config->operation & SPI_TRANSFER_LSB) {
		data->fsp_cfg.bit_order = SPI_BIT_ORDER_LSB_FIRST;
	} else {
		data->fsp_cfg.bit_order = SPI_BIT_ORDER_MSB_FIRST;
	}

	/*
	 * Master clock divider.  R_SCI_B_SPI_CalculateBitrate() returns the
	 * largest achievable bitrate <= requested, so an over-spec request
	 * (e.g. the 10 MHz the example asks for) is simply clamped to the SCI
	 * ceiling rather than rejected -- no artificial cap here.
	 */
	if (!(config->operation & SPI_OP_MODE_SLAVE)) {
		fsp_err = R_SCI_B_SPI_CalculateBitrate(config->frequency,
						       data->fsp_ext_cfg.clock_source,
						       &data->fsp_ext_cfg.clk_div);
		if (fsp_err != FSP_SUCCESS) {
			LOG_ERR("Bitrate %u not achievable: %d", config->frequency, fsp_err);
			return -EINVAL;
		}
	}

	data->fsp_cfg.p_extend = &data->fsp_ext_cfg;
	data->fsp_cfg.p_callback = rz_sci_b_spi_callback;
	data->fsp_cfg.p_context = (void *)dev;

	if (data->fsp_ctrl.open != 0) {
		R_SCI_B_SPI_Close(&data->fsp_ctrl);
	}

	fsp_err = R_SCI_B_SPI_Open(&data->fsp_ctrl, &data->fsp_cfg);
	if (fsp_err != FSP_SUCCESS) {
		LOG_ERR("R_SCI_B_SPI_Open error: %d", fsp_err);
		return -EINVAL;
	}

	data->ctx.config = config;

	return 0;
}

/*
 * Recover a wedged SCI-SPI controller after a transfer error or timeout.
 *
 * The FSP error ISR (sci_b_spi_eri_isr) clears only TIE/RIE/TEIE on a bus
 * error -- it leaves CCR0.TE and RE SET, and because it also clears TEIE the
 * transmit-end ISR (sci_b_spi_tei_isr, the sole place TE/RE are cleared) never
 * fires.  TE/RE then stay latched, so r_sci_b_spi_write_read_common()'s
 * "TE and RE must be zero" guard (r_sci_b_spi.c:809) rejects every subsequent
 * transfer with FSP_ERR_IN_USE -> -EIO forever: CS/SCK stop toggling after the
 * first failure and the link is dead until reboot.
 *
 * Recovery has to restore the channel to the same clean state R_SCI_B_SPI_Open's
 * hw_config leaves it in -- because configure() keeps the cached spi_config and
 * fast-returns, hw_config never re-runs, so recover() is the ONLY place that
 * re-cleans the RX path between transfers.  It therefore:
 *   - clears TE/RE AND every transfer-interrupt enable (TIE/RIE/TEIE) under
 *     irq_lock, so the read-modify-write can't race the rxi/txi/tei ISRs and no
 *     stray ISR is left armed (matters on the -ETIMEDOUT path, where the
 *     transfer may still be nominally live);
 *   - drains RDR and clears the receive-data-ready + error status (RDRFC/ORERC/
 *     FERC/PERC); the error being recovered is an RX overrun, so a stale byte
 *     sits in RDR with RDRF latched -- left in place it shifts the very next
 *     transfer by one frame (CRC fail -> STATUS_IO, the symptom this fix kills);
 *   - zeroes the FSP transfer counters so a rxi that was already latched cannot
 *     write a late byte into the previous caller's buffer via the stale p_dest
 *     (write_read_common re-inits count/tx/rx/p_src/p_dest on the next call).
 *
 * It deliberately does NOT Close()/re-Open(): R_SCI_B_SPI_Close() calls
 * R_BSP_MODULE_STOP(FSP_IP_SCI, channel), and on RZ/V2N re-starting SCI7's
 * module clock from the CM33 is unreliable (the A55 owns the CPG module-stop
 * gate), so a Close could leave SCI7 unreachable.  The channel stays open; the
 * next transfer simply re-arms TE/RE via r_sci_b_spi_start_transfer().
 */
static void rz_sci_b_spi_recover(struct rz_sci_b_spi_data *data)
{
	R_SCI_B0_Type *p_reg = data->fsp_ctrl.p_reg;
	unsigned int key;

	if (data->fsp_ctrl.open == 0) {
		return;
	}

	key = irq_lock();

	p_reg->CCR0 &= (uint32_t)~(R_SCI_B0_CCR0_TE_Msk | R_SCI_B0_CCR0_RE_Msk |
				   R_SCI_B0_CCR0_TIE_Msk | R_SCI_B0_CCR0_RIE_Msk |
				   R_SCI_B0_CCR0_TEIE_Msk);

	(void)p_reg->RDR;
	p_reg->CFCLR = R_SCI_B0_CFCLR_RDRFC_Msk | R_SCI_B0_CFCLR_ORERC_Msk |
		       R_SCI_B0_CFCLR_FERC_Msk | R_SCI_B0_CFCLR_PERC_Msk;

	data->fsp_ctrl.count = 0U;
	data->fsp_ctrl.rx_count = 0U;
	data->fsp_ctrl.tx_count = 0U;

	irq_unlock(key);
}

static int transceive(const struct device *dev, const struct spi_config *config,
		      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
		      bool asynchronous, spi_callback_t cb, void *userdata)
{
	struct rz_sci_b_spi_data *data = dev->data;
	fsp_err_t fsp_err;
	int ret;

	if (!tx_bufs && !rx_bufs) {
		return 0;
	}

	spi_context_lock(&data->ctx, asynchronous, cb, userdata, config);

	ret = rz_sci_b_spi_configure(dev, config);
	if (ret) {
		goto end;
	}

	/* SCI Simple-SPI is byte-oriented: data frame size is always 1. */
	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	if ((!spi_context_tx_buf_on(&data->ctx)) && (!spi_context_rx_buf_on(&data->ctx))) {
		goto end;
	}

	/* Re-own SCK7/CS right before the transaction -- beats any Linux port-9
	 * clobber that landed since init (the bench poke re-asserted continuously). */
	alp_v2n_pins_assert();
	spi_context_cs_control(&data->ctx, true);
	alp_v2n_cs_assert();

	data->data_len = spi_context_max_continuous_chunk(&data->ctx);

	if (data->ctx.rx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Write(&data->fsp_ctrl, data->ctx.tx_buf, data->data_len,
					    SPI_BIT_WIDTH_8_BITS);
	} else if (data->ctx.tx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Read(&data->fsp_ctrl, data->ctx.rx_buf, data->data_len,
					   SPI_BIT_WIDTH_8_BITS);
	} else {
		fsp_err = R_SCI_B_SPI_WriteRead(&data->fsp_ctrl, data->ctx.tx_buf, data->ctx.rx_buf,
						data->data_len, SPI_BIT_WIDTH_8_BITS);
	}

	if (fsp_err != FSP_SUCCESS) {
		spi_context_cs_control(&data->ctx, false);
		alp_v2n_cs_deassert();
		/* FSP_ERR_IN_USE here means a prior error left TE/RE latched;
		 * clear them so the NEXT ping is not rejected too. */
		rz_sci_b_spi_recover(data);
		ret = -EIO;
		goto end;
	}

	ret = spi_context_wait_for_completion(&data->ctx);

	if (ret < 0) {
		/* A bus error (-EIO via the FSP error callback) or a completion
		 * timeout (-ETIMEDOUT; the master-mode wait is bounded by
		 * CONFIG_SPI_COMPLETION_TIMEOUT_TOLERANCE) leaves the controller
		 * wedged with TE/RE latched.  Recover so the next ping re-arms
		 * cleanly instead of returning FSP_ERR_IN_USE forever. */
		rz_sci_b_spi_recover(data);
	}

#ifdef CONFIG_SPI_SLAVE
	if (spi_context_is_slave(&data->ctx) && !ret) {
		ret = data->ctx.recv_frames;
	}
#endif /* CONFIG_SPI_SLAVE */

end:
	spi_context_release(&data->ctx, ret);

	return ret;
}

static int rz_sci_b_spi_transceive(const struct device *dev, const struct spi_config *config,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs)
{
	return transceive(dev, config, tx_bufs, rx_bufs, false, NULL, NULL);
}

#ifdef CONFIG_SPI_ASYNC
static int rz_sci_b_spi_transceive_async(const struct device *dev, const struct spi_config *config,
					 const struct spi_buf_set *tx_bufs,
					 const struct spi_buf_set *rx_bufs, spi_callback_t cb,
					 void *userdata)
{
	return transceive(dev, config, tx_bufs, rx_bufs, true, cb, userdata);
}
#endif /* CONFIG_SPI_ASYNC */

static int rz_sci_b_spi_release(const struct device *dev, const struct spi_config *config)
{
	struct rz_sci_b_spi_data *data = dev->data;

	if (!spi_context_configured(&data->ctx, config)) {
		return -EINVAL;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, rz_sci_b_spi_driver_api) = {
	.transceive = rz_sci_b_spi_transceive,
#ifdef CONFIG_SPI_ASYNC
	.transceive_async = rz_sci_b_spi_transceive_async,
#endif /* CONFIG_SPI_ASYNC */
	.release = rz_sci_b_spi_release,
};

static int rz_sci_b_spi_init(const struct device *dev)
{
	const struct rz_sci_b_spi_config *config = dev->config;
	struct rz_sci_b_spi_data *data = dev->data;
	int ret;

	/*
	 * RZ/V2N SCI operation clock is P5CLK -- selected via PCLK, the only
	 * source wired to SCI on this SoC (see r_sci_b_spi_cfg.h).
	 */
	data->fsp_ext_cfg.clock_source = SCI_B_SPI_SOURCE_CLOCK_PCLK;
	data->fsp_ext_cfg.rx_sampling_delay = SCI_B_SPI_RX_SAMPLING_DELAY_CYCLES_0;
	data->fsp_ext_cfg.tx_fifo_trigger = SCI_B_SPI_TX_FIFO_TRIGGER_DISABLED;

	ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}

	/* Own SCK7(P96)/CS(P97): pinctrl above set the mux/PFC; this holds PMC9
	 * bit6 + PM9 P97 against Linux's shared port-9 (P94 SD1_CD) clobber. */
	alp_v2n_pins_assert();

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SCI_NODE(idx) DT_INST_PARENT(idx)

#ifdef CONFIG_DT_HAS_RENESAS_RZ_ICU_V2_ENABLED
#define EVENT_SCI(channel, IRQ_NAME) CONCAT(ELC_EVENT_SCI, channel, _##IRQ_NAME)
#define RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, irq_name, IRQ_NAME)                                      \
	icu_connect_irq_event(DT_IRQ_BY_NAME(SCI_NODE(n), irq_name, irq),                          \
			      (EVENT_SCI(DT_PROP(SCI_NODE(n), channel), IRQ_NAME)))
#else
#define RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, irq_name, IRQ_NAME)
#endif /* CONFIG_DT_HAS_RENESAS_RZ_ICU_V2_ENABLED */

#ifdef CONFIG_CPU_CORTEX_M
#define RZ_SCI_B_SPI_IRQ_FLAGS(n, irq_name) 0
#else
#define RZ_SCI_B_SPI_IRQ_FLAGS(n, irq_name) DT_IRQ_BY_NAME(SCI_NODE(n), irq_name, flags)
#endif

#define RZ_SCI_B_SPI_IRQ_CONNECT(n, irq_name, isr)                                                 \
	do {                                                                                       \
		IRQ_CONNECT(DT_IRQ_BY_NAME(SCI_NODE(n), irq_name, irq),                            \
			    DT_IRQ_BY_NAME(SCI_NODE(n), irq_name, priority), isr,                  \
			    DEVICE_DT_INST_GET(n), RZ_SCI_B_SPI_IRQ_FLAGS(n, irq_name));           \
		irq_enable(DT_IRQ_BY_NAME(SCI_NODE(n), irq_name, irq));                            \
	} while (0)

#define RZ_SCI_B_SPI_CONFIG_FUNC(n)                                                                \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, rxi, RXI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, txi, TXI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, tei, TEI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, eri, ERI);                                               \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, rxi, sci_b_spi_rxi_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, txi, sci_b_spi_txi_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, tei, sci_b_spi_tei_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, eri, sci_b_spi_eri_isr);

#define RZ_SCI_B_SPI_INIT(n)                                                                       \
	PINCTRL_DT_DEFINE(SCI_NODE(n));                                                            \
                                                                                                   \
	static const struct rz_sci_b_spi_config rz_sci_b_spi_config_##n = {                        \
		.pcfg = PINCTRL_DT_DEV_CONFIG_GET(SCI_NODE(n)),                                    \
	};                                                                                         \
                                                                                                   \
	static struct rz_sci_b_spi_data rz_sci_b_spi_data_##n = {                                  \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)                               \
			SPI_CONTEXT_INIT_LOCK(rz_sci_b_spi_data_##n, ctx),                         \
		SPI_CONTEXT_INIT_SYNC(rz_sci_b_spi_data_##n, ctx),                                 \
		.fsp_cfg =                                                                         \
			{                                                                          \
				.channel = DT_PROP(SCI_NODE(n), channel),                          \
				.rxi_irq = DT_IRQ_BY_NAME(SCI_NODE(n), rxi, irq),                  \
				.rxi_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), rxi, priority),             \
				.txi_irq = DT_IRQ_BY_NAME(SCI_NODE(n), txi, irq),                  \
				.txi_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), txi, priority),             \
				.tei_irq = DT_IRQ_BY_NAME(SCI_NODE(n), tei, irq),                  \
				.tei_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), tei, priority),             \
				.eri_irq = DT_IRQ_BY_NAME(SCI_NODE(n), eri, irq),                  \
				.eri_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), eri, priority),             \
				.p_transfer_tx = NULL,                                             \
				.p_transfer_rx = NULL,                                             \
			},                                                                         \
	};                                                                                         \
                                                                                                   \
	static int rz_sci_b_spi_init_##n(const struct device *dev)                                 \
	{                                                                                          \
		int err = rz_sci_b_spi_init(dev);                                                  \
                                                                                                   \
		if (err != 0) {                                                                    \
			return err;                                                                \
		}                                                                                  \
		RZ_SCI_B_SPI_CONFIG_FUNC(n);                                                        \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, rz_sci_b_spi_init_##n, NULL, &rz_sci_b_spi_data_##n,               \
			      &rz_sci_b_spi_config_##n, POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,      \
			      &rz_sci_b_spi_driver_api);

DT_INST_FOREACH_STATUS_OKAY(RZ_SCI_B_SPI_INIT)
