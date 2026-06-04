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
/* CS OWNERSHIP (silicon-resolved 2026-06-03): the direct latch below is THE
 * chip-select owner on RZ/V2N.  Setting this to 0 routes CS through the
 * standard spi_context GPIO path (gpio_rz) instead and skips the
 * per-transceive pin re-own -- tested on silicon WITH the carrier P94/SD1_CD
 * clobber fix deployed, and it FAILS: zero bytes reach the GD32.  Root cause:
 * the gpio_rz configure/output path leaves PM9's P97 field in the high-only
 * drive mode (0b01; empirically swept -- the pad needs 0b11), and the alp SPI
 * backend's gpio_pin_configure_dt() at alp_spi_open() re-applies that broken
 * mode even with Linux's port-9 clobber gone -- which is also why the
 * per-transceive alp_v2n_pins_assert() below must stay.  Re-test with 0 only
 * after the gpio_rz output path is fixed for this pad. */
#ifndef ALP_V2N_DIRECT_CS_SHIM
#define ALP_V2N_DIRECT_CS_SHIM 1
#endif

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
#if ALP_V2N_DIRECT_CS_SHIM
	sys_write8(sys_read8(ALP_V2N_P9_LATCH) & (uint8_t)~ALP_V2N_CS_MASK, ALP_V2N_P9_LATCH);
	k_busy_wait(ALP_V2N_CS_SETUP_US);
#endif
}
static ALWAYS_INLINE void alp_v2n_cs_deassert(void)
{
#if ALP_V2N_DIRECT_CS_SHIM
	k_busy_wait(ALP_V2N_CS_HOLD_US);
	sys_write8(sys_read8(ALP_V2N_P9_LATCH) | ALP_V2N_CS_MASK, ALP_V2N_P9_LATCH);
#endif
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

/* ── SCI7 25 MHz data path: zero-interrupt POLLED engine ─────────────────────
 * The SCI Simple-SPI master is byte-per-event with a 1-deep TDR/RDR.
 * Interrupt-per-byte tops out around 5-8 MHz (IRQ entry/exit latency), and
 * the DMAC fast path is silicon-blocked (see ALP_V2N_SCI7_DMAC below), so the
 * 25 MHz link uses a tight TDRE/RDRF-polled loop instead: at 25 MHz a frame
 * is 320 ns while a CSR poll + TDR/RDR access pair is well under that on the
 * 200 MHz CM33, and -- decisively -- the MASTER paces SCK (the SCI only
 * clocks while TDR is fed), so a late poll merely stretches the inter-byte
 * gap; the GD32 slave's DMA capture is indifferent to gaps.  Frames are
 * <= 69 bytes, so worst-case CPU time is tens of microseconds per
 * transaction at the example's Hz-rate cadence -- negligible for the
 * supervisor link, with zero per-byte interrupts. */
#define ALP_V2N_POLL_GUARD 100000u /* per-flag spin bound (~ms at 25 MHz) */

/* ── SCI7 DMA fast path: PRESERVED BUT DISABLED (silicon-blocked) ────────────
 * Set to 1 (plus SCI_B_SPI_CFG_DMA_SUPPORT_ENABLE=1 in r_sci_b_spi_cfg.h) to
 * re-wire the FSP transfer interface to the RZ/V2N MCPU DMAC (DMAC0) via the
 * rzv FSP r_dmac_b.  Bring-up findings 2026-06-03 (scope + ICU/DMAC
 * register post-mortems via A55 /dev/mem):
 *   - DM4SEL0 correctly routes trigger 168 (SCI7 TXI; the bsp_dmac.h enum
 *     values are the right DkRQ_SEL numbers -- the HW manual Table 4.6-23
 *     column 203/205 routes NOTHING),
 *   - with CCR0.TE+TIE armed and CSR.TDRE high the channel still never
 *     streams: at most one beat per arm, CHSTAT parks at RQST(+SUS),
 *     EN never reads 1 -- identical across ack modes (MASK_DACK_OUTPUT,
 *     BUS_CYCLE) and detection modes (edge, HIGH_LEVEL),
 *   - net: zero SCK on the wire.  FSP r_dmac_b + r_sci_b_spi pairing is
 *     unvalidated by the vendor on this SoC; raise with Renesas before
 *     re-enabling.  DMAC0 ch0/ch1, NVIC 89/90; kernel AMP patch already
 *     holds the DMAC0 clocks. */
#ifndef ALP_V2N_SCI7_DMAC
#define ALP_V2N_SCI7_DMAC 0
#endif

#if ALP_V2N_SCI7_DMAC
#include "r_dmac_b.h"
#endif

/* FSP r_sci_b_spi interrupt service routines (vendored module). */
extern void sci_b_spi_rxi_isr(void);
extern void sci_b_spi_txi_isr(void);
extern void sci_b_spi_tei_isr(void);
extern void sci_b_spi_eri_isr(void);

#if ALP_V2N_SCI7_DMAC
/* FSP r_dmac_b transfer-end ISR + the r_sci_b_spi DMA completion hooks the
 * DMAC callbacks MUST invoke (they arm CCR0.TEIE so tei fires
 * SPI_EVENT_TRANSFER_COMPLETE; the FSP does not auto-wire them). */
extern void dmac_b_int_isr(void);
extern void sci_b_spi_rx_dmac_callback(sci_b_spi_instance_ctrl_t const * const p_ctrl);
extern void sci_b_spi_tx_dmac_callback(sci_b_spi_instance_ctrl_t const * const p_ctrl);

#define ALP_V2N_SCI7_DMAC_UNIT  0 /* DMAC0 = MCPU (CM33) DMAC            */
#define ALP_V2N_SCI7_DMAC_RX_CH 0 /* DMAINT0 -> NVIC 89                   */
#define ALP_V2N_SCI7_DMAC_TX_CH 1 /* DMAINT1 -> NVIC 90                   */
#define ALP_V2N_DMAC_TRIGGER_SCI7_RXI DMAC_TRIGGER_EVENT_SCI7_RXI
#define ALP_V2N_DMAC_TRIGGER_SCI7_TXI DMAC_TRIGGER_EVENT_SCI7_TXI
#endif /* ALP_V2N_SCI7_DMAC */

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
	default:
		/* Any unrecognised FSP event during a transfer is an error too:
		 * complete with -EIO (the waiter recovers via
		 * rz_sci_b_spi_recover()) instead of leaving the caller to ride
		 * out the completion timeout with CS still asserted. */
		spi_context_cs_control(&data->ctx, false);
		alp_v2n_cs_deassert();
		spi_context_complete(&data->ctx, dev, -EIO);
		break;
	}
}

#if ALP_V2N_SCI7_DMAC
/* DMAC completion bridges: the r_dmac_b channel-end ISR calls these (via the
 * extended cfg p_callback); they forward to the r_sci_b_spi DMA hooks that
 * disarm TIE/RIE and arm TEIE, after which the SCI tei ISR raises
 * SPI_EVENT_TRANSFER_COMPLETE through the normal callback path. */
static void rz_sci_b_spi_rx_dmac_cb(dmac_b_callback_args_t *p_args)
{
	const struct device *dev = p_args->p_context;
	struct rz_sci_b_spi_data *data = dev->data;

	sci_b_spi_rx_dmac_callback(&data->fsp_ctrl);
}

static void rz_sci_b_spi_tx_dmac_cb(dmac_b_callback_args_t *p_args)
{
	const struct device *dev = p_args->p_context;
	struct rz_sci_b_spi_data *data = dev->data;

	sci_b_spi_tx_dmac_callback(&data->fsp_ctrl);
}
#endif /* ALP_V2N_SCI7_DMAC */

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

#if ALP_V2N_SCI7_DMAC
	/* Quiesce the DMAC channels FIRST so no further RDR/TDR traffic lands
	 * while the SCI is being cleaned up; an armed channel left over from the
	 * failed transfer must not fire into the next one.  The next transfer's
	 * reconfigure()/reset() re-arms them from scratch.  (Outside the
	 * irq_lock: R_DMAC_B_Disable contains short register-wait spins.) */
	if (data->fsp_cfg.p_transfer_rx != NULL) {
		(void)R_DMAC_B_Disable(data->fsp_cfg.p_transfer_rx->p_ctrl);
	}
	if (data->fsp_cfg.p_transfer_tx != NULL) {
		(void)R_DMAC_B_Disable(data->fsp_cfg.p_transfer_tx->p_ctrl);
	}
#endif /* ALP_V2N_SCI7_DMAC */

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

/*
 * Zero-interrupt polled full-duplex engine (the 25 MHz production path; see
 * the data-path note at the top of the file).  Walks the whole spi_context
 * buffer set frame by frame: feed TDR on TDRE, collect RDR on RDRF (the SCI
 * auto-clears both flags on the data-register access, exactly as the FSP's
 * own ISRs rely on).  The configure() step has already programmed the bit
 * rate (CCR2) and frame format (CCR3) via R_SCI_B_SPI_Open; this engine only
 * drives TE/RE around the transaction, so rz_sci_b_spi_recover() remains
 * valid for any error exit.  Synchronous by design -- with master-paced SCK
 * the whole worst-case frame (69 B) is ~25 us of wall time at 25 MHz.
 */
static int rz_sci_b_spi_xfer_polled(struct rz_sci_b_spi_data *data)
{
	R_SCI_B0_Type *p_reg = data->fsp_ctrl.p_reg;
	struct spi_context *ctx = &data->ctx;
	uint32_t guard;

	/* Engage the transceiver.  TE+RE together: the FSP warns RE-without-TE
	 * free-runs SCK, and TE-only would discard the (full-duplex) returns. */
	p_reg->CCR0 |= (R_SCI_B0_CCR0_TE_Msk | R_SCI_B0_CCR0_RE_Msk);

	while (spi_context_tx_on(ctx) || spi_context_rx_on(ctx)) {
		const uint8_t out = spi_context_tx_buf_on(ctx) ? *ctx->tx_buf : 0x00U;
		uint8_t in;

		guard = ALP_V2N_POLL_GUARD;
		while (!(p_reg->CSR & R_SCI_B0_CSR_TDRE_Msk)) {
			if (--guard == 0U) {
				goto stalled;
			}
		}
		p_reg->TDR = (0xFFFFFF00UL | out);

		guard = ALP_V2N_POLL_GUARD;
		while (!(p_reg->CSR & R_SCI_B0_CSR_RDRF_Msk)) {
			if (--guard == 0U) {
				goto stalled;
			}
		}
		in = (uint8_t)(p_reg->RDR & 0xFFU);

		if (spi_context_rx_buf_on(ctx)) {
			*ctx->rx_buf = in;
		}
		spi_context_update_tx(ctx, 1, 1);
		spi_context_update_rx(ctx, 1, 1);
	}

	/* Let the final frame drain out of the shifter before dropping TE --
	 * deasserting CS mid-shift would truncate the slave's last byte. */
	guard = ALP_V2N_POLL_GUARD;
	while (!(p_reg->CSR & R_SCI_B0_CSR_TEND_Msk)) {
		if (--guard == 0U) {
			goto stalled;
		}
	}

	p_reg->CCR0 &= (uint32_t)~(R_SCI_B0_CCR0_TE_Msk | R_SCI_B0_CCR0_RE_Msk);

	return 0;

stalled:
	/* A wedged flag (clock loss, gate closure mid-transfer) -- clean the
	 * controller so the next attempt re-arms; the protocol layer retries. */
	rz_sci_b_spi_recover(data);
	return -EIO;
}

static int transceive(const struct device *dev, const struct spi_config *config,
		      const struct spi_buf_set *tx_bufs, const struct spi_buf_set *rx_bufs,
		      bool asynchronous, spi_callback_t cb, void *userdata)
{
	struct rz_sci_b_spi_data *data = dev->data;
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

#if ALP_V2N_DIRECT_CS_SHIM
	/* Re-own SCK7/CS right before the transaction -- beats any Linux port-9
	 * clobber that landed since init (the bench poke re-asserted continuously). */
	alp_v2n_pins_assert();
#endif
	spi_context_cs_control(&data->ctx, true);
	alp_v2n_cs_assert();

	ret = rz_sci_b_spi_xfer_polled(data);

	spi_context_cs_control(&data->ctx, false);
	alp_v2n_cs_deassert();

	if (asynchronous) {
		/* The engine is synchronous; satisfy the async contract by
		 * completing immediately (fires the user callback). */
		spi_context_complete(&data->ctx, dev, ret);
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

#if ALP_V2N_SCI7_DMAC
/* DMAC0 channel-end vectors (CM33 NVIC 89+ch).  The FSP enables them
 * (R_BSP_IrqCfgEnable in the DMAC enable path) and resolves the channel
 * ctrl in dmac_b_int_isr via the registered ISR context. */
#define RZ_SCI_B_SPI_DMAC_CONNECT(n)                                                               \
	IRQ_CONNECT(DMAC_B0_DMAINT0_IRQn + ALP_V2N_SCI7_DMAC_RX_CH,                                \
		    DT_IRQ_BY_NAME(SCI_NODE(n), rxi, priority), dmac_b_int_isr,                    \
		    DEVICE_DT_INST_GET(n), 0);                                                     \
	IRQ_CONNECT(DMAC_B0_DMAINT0_IRQn + ALP_V2N_SCI7_DMAC_TX_CH,                                \
		    DT_IRQ_BY_NAME(SCI_NODE(n), txi, priority), dmac_b_int_isr,                    \
		    DEVICE_DT_INST_GET(n), 0);
#else
#define RZ_SCI_B_SPI_DMAC_CONNECT(n)
#endif /* ALP_V2N_SCI7_DMAC */

#define RZ_SCI_B_SPI_CONFIG_FUNC(n)                                                                \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, rxi, RXI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, txi, TXI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, tei, TEI);                                               \
	RZ_SCI_B_SPI_CONNECT_IRQ_EVENT(n, eri, ERI);                                               \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, rxi, sci_b_spi_rxi_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, txi, sci_b_spi_txi_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, tei, sci_b_spi_tei_isr);                                       \
	RZ_SCI_B_SPI_IRQ_CONNECT(n, eri, sci_b_spi_eri_isr);                                       \
	RZ_SCI_B_SPI_DMAC_CONNECT(n)

#if ALP_V2N_SCI7_DMAC
/* Per-instance DMAC0 transfer pair (RX then TX).  transfer_info_t stays
 * mutable (the FSP SCI module rewrites src/dest/length per transfer); the
 * cfg/extended-cfg/instance structs are const.  The channel-end NVIC vector
 * is enabled by the FSP itself (R_BSP_IrqCfgEnable inside the DMAC enable
 * path, which also registers the ISR context); the glue only IRQ_CONNECTs. */
#define RZ_SCI_B_SPI_DMAC_DEFINE(n, dir, ch, trigger, reqdir, cb)                                  \
	static dmac_b_instance_ctrl_t rz_sci_b_spi_##dir##_dmac_ctrl_##n;                          \
	static transfer_info_t rz_sci_b_spi_##dir##_dmac_info_##n;                                 \
	static const dmac_b_extended_cfg_t rz_sci_b_spi_##dir##_dmac_ext_##n = {                   \
		.unit = ALP_V2N_SCI7_DMAC_UNIT,                                                    \
		.channel = (ch),                                                                   \
		.dmac_int_irq = (IRQn_Type)(DMAC_B0_DMAINT0_IRQn + (ch)),                          \
		.dmac_int_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), rxi, priority),                        \
		.activation_source = (trigger),                                                    \
		/* BUS_CYCLE ack: the on-chip TDRE/RDRF requests are flow-controlled               \
		 * by the data-register access itself -- the bus cycle IS the                      \
		 * acknowledge.  With the DACK masked instead, the level request is                \
		 * never acknowledged and the channel parks SUSPENDED after its                    \
		 * first beat (CHSTAT.SUS=1, scope-confirmed zero SCK, 2026-06-03).                \
		 * Matches the Linux rz-dmac convention for peripheral slaves. */                  \
		.ack_mode = DMAC_B_ACK_MODE_BUS_CYCLE_MODE,                                        \
		/* external pin mode is don't-care for internal (peripheral)                       \
		 * triggers.  internal_detection MUST be HIGH_LEVEL (CHCFG                         \
		 * LVL|HIEN): the SCI TDRE/RDRF requests are level lines, and the                  \
		 * FSP arms the transfer BEFORE start_transfer() sets CCR0.TE --                   \
		 * with edge/no detection the only TDRE edge is consumed while                     \
		 * TE=0 (one byte vanishes into a non-transmitting TDR) and no new                 \
		 * edge ever comes, freezing the stream at CRSA=N0SA+1 with zero                   \
		 * SCK (seen on silicon 2026-06-03, scope-confirmed).  Level                       \
		 * detection re-fires as long as the line is active.  This matches                 \
		 * the Linux rz-dmac convention for on-chip peripheral requests. */                \
		.external_detection_mode = DMAC_B_EXTERNAL_DETECTION_LOW_LEVEL,                    \
		.internal_detection_mode = DMAC_B_INTERNAL_DETECTION_HIGH_LEVEL,                   \
		.activation_request_source_select = (reqdir),                                      \
		.dmac_mode = DMAC_B_MODE_SELECT_REGISTER,                                          \
		.continuous_setting = DMAC_B_CONTINUOUS_SETTING_TRANSFER_ONCE,                     \
		.transfer_interval = 0,                                                            \
		.channel_scheduling = DMAC_B_CHANNEL_SCHEDULING_FIXED,                             \
		.p_callback = (cb),                                                                \
		.p_context = (void *)DEVICE_DT_INST_GET(n),                                        \
	};                                                                                         \
	static const transfer_cfg_t rz_sci_b_spi_##dir##_dmac_cfg_##n = {                          \
		.p_info = &rz_sci_b_spi_##dir##_dmac_info_##n,                                     \
		.p_extend = &rz_sci_b_spi_##dir##_dmac_ext_##n,                                    \
	};                                                                                         \
	static const transfer_instance_t rz_sci_b_spi_##dir##_transfer_##n = {                     \
		.p_ctrl = &rz_sci_b_spi_##dir##_dmac_ctrl_##n,                                     \
		.p_cfg = &rz_sci_b_spi_##dir##_dmac_cfg_##n,                                       \
		.p_api = &g_transfer_on_dmac_b,                                                    \
	};

#define RZ_SCI_B_SPI_DMAC_PAIR(n)                                                                  \
	RZ_SCI_B_SPI_DMAC_DEFINE(n, rx, ALP_V2N_SCI7_DMAC_RX_CH,                                   \
				 ALP_V2N_DMAC_TRIGGER_SCI7_RXI,                                    \
				 DMAC_B_REQUEST_DIRECTION_DESTINATION_MODULE,                      \
				 rz_sci_b_spi_rx_dmac_cb)                                          \
	RZ_SCI_B_SPI_DMAC_DEFINE(n, tx, ALP_V2N_SCI7_DMAC_TX_CH,                                   \
				 ALP_V2N_DMAC_TRIGGER_SCI7_TXI,                                    \
				 DMAC_B_REQUEST_DIRECTION_SOURCE_MODULE,                           \
				 rz_sci_b_spi_tx_dmac_cb)
#define RZ_SCI_B_SPI_TRANSFER_TX(n) (&rz_sci_b_spi_tx_transfer_##n)
#define RZ_SCI_B_SPI_TRANSFER_RX(n) (&rz_sci_b_spi_rx_transfer_##n)
/* DMA mode: rxi/txi stay OFF the NVIC (-1; allowed by the FSP asserts when
 * transfers are supplied) -- on this SoC the SCI events fan out to BOTH the
 * DMAC trigger mux and the NVIC, and the RA-derived rxi/txi ISRs declare the
 * transfer done early (tx_count pre-set == count arms TEIE on the first
 * TDR-empty), truncating frames. */
#define RZ_SCI_B_SPI_RXI_IRQ(n) ((IRQn_Type)-1)
#define RZ_SCI_B_SPI_TXI_IRQ(n) ((IRQn_Type)-1)
#else
#define RZ_SCI_B_SPI_DMAC_PAIR(n)
#define RZ_SCI_B_SPI_TRANSFER_TX(n) NULL
#define RZ_SCI_B_SPI_TRANSFER_RX(n) NULL
/* Polled engine: the FSP interrupt machinery is configured (the asserts
 * require valid rxi/txi vectors when no transfers are supplied) but never
 * armed -- the polled engine drives TE/RE itself and the FSP Write/Read
 * entry points are not used, so TIE/RIE never set and these ISRs stay
 * silent. */
#define RZ_SCI_B_SPI_RXI_IRQ(n) DT_IRQ_BY_NAME(SCI_NODE(n), rxi, irq)
#define RZ_SCI_B_SPI_TXI_IRQ(n) DT_IRQ_BY_NAME(SCI_NODE(n), txi, irq)
#endif /* ALP_V2N_SCI7_DMAC */

#define RZ_SCI_B_SPI_INIT(n)                                                                       \
	PINCTRL_DT_DEFINE(SCI_NODE(n));                                                            \
                                                                                                   \
	RZ_SCI_B_SPI_DMAC_PAIR(n)                                                                  \
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
				.rxi_irq = RZ_SCI_B_SPI_RXI_IRQ(n),                                \
				.rxi_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), rxi, priority),             \
				.txi_irq = RZ_SCI_B_SPI_TXI_IRQ(n),                                \
				.txi_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), txi, priority),             \
				.tei_irq = DT_IRQ_BY_NAME(SCI_NODE(n), tei, irq),                  \
				.tei_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), tei, priority),             \
				.eri_irq = DT_IRQ_BY_NAME(SCI_NODE(n), eri, irq),                  \
				.eri_ipl = DT_IRQ_BY_NAME(SCI_NODE(n), eri, priority),             \
				.p_transfer_tx = RZ_SCI_B_SPI_TRANSFER_TX(n),                      \
				.p_transfer_rx = RZ_SCI_B_SPI_TRANSFER_RX(n),                      \
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
