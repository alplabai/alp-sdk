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
 * and are dropped/misaligned (seen on silicon as a 2-of-4-byte, A5-84 capture).
 *
 * Sizing: the slave's CS-EXTI work is ~0.2-1 us (EXTI dispatch + DMA re-arm
 * register writes at 216 MHz).  Bring-up used 60/30 us of paranoia; the
 * 2026-06-04 bench ladder (link-bench latency suite + soak regression)
 * walked 10/5 us then 3/2 us -- still ~2-3x the estimated floor.  SLOW
 * HANDLERS (ADC conversion, TRNG conditioning, OTA FMC programming) are not
 * these gaps' job: the gd32g553 host driver's reply re-read schedule covers
 * them.
 *
 * Timing source: a raw SysTick down-counter spin, NOT k_busy_wait().  The
 * link-bench calibration probe measured k_busy_wait(10) at ~15 us on this
 * platform -- its k_cycle_get_32() poll (irq-locked 64-bit SysTick
 * accumulation) is microseconds-coarse, so every call overshoots by about
 * one poll period.  Four such waits per transaction made the kernel's own
 * spin half the link's fixed overhead.  SysTick is the kernel timebase, so
 * it is guaranteed to be running whenever a transfer executes; one VAL read
 * costs tens of nanoseconds, giving sub-100 ns spin granularity.  Spins
 * here are well under the reload period, so at most one wrap can land
 * mid-spin -- the modular (last - now) % reload delta handles it.  Under
 * TICKLESS_KERNEL the timer driver reprograms LOAD dynamically, so a tick
 * ISR landing mid-spin can distort one delta; per-sample deltas are
 * therefore capped, which biases ONLY toward a longer spin (the windows
 * are minimums -- running long is always safe, running short is not). */
#define ALP_V2N_CS_SETUP_US 3
#define ALP_V2N_CS_HOLD_US  2
#define ALP_V2N_CYC_PER_US  (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U)
static ALWAYS_INLINE void alp_v2n_spin_us(uint32_t us)
{
	const uint32_t reload = SysTick->LOAD + 1U;
	const uint32_t target = us * ALP_V2N_CYC_PER_US;
	uint32_t last = SysTick->VAL;
	uint32_t elapsed = 0U;

	while (elapsed < target) {
		const uint32_t now = SysTick->VAL;
		/* Down-counter: delta = (last - now) mod reload. */
		uint32_t d = (last >= now) ? (last - now) : (last + reload - now);

		/* Poll-to-poll is tens of cycles; anything bigger is an ISR
		 * or a tickless LOAD rewrite mid-spin -- count at most 1 us
		 * of it so a distorted sample cannot cut the window short. */
		if (d > ALP_V2N_CYC_PER_US) {
			d = ALP_V2N_CYC_PER_US;
		}
		elapsed += d;
		last = now;
	}
}
static ALWAYS_INLINE void alp_v2n_cs_assert(void)
{
#if ALP_V2N_DIRECT_CS_SHIM
	sys_write8(sys_read8(ALP_V2N_P9_LATCH) & (uint8_t)~ALP_V2N_CS_MASK, ALP_V2N_P9_LATCH);
	alp_v2n_spin_us(ALP_V2N_CS_SETUP_US);
#endif
}
static ALWAYS_INLINE void alp_v2n_cs_deassert(void)
{
#if ALP_V2N_DIRECT_CS_SHIM
	alp_v2n_spin_us(ALP_V2N_CS_HOLD_US);
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
 * Interrupt-per-byte tops out around 5-8 MHz (IRQ entry/exit latency), and
 * the DMAC fast path is silicon-blocked (see ALP_V2N_SCI7_DMAC below), so the
 * 25 MHz link uses a polled engine.  Polling the per-byte flags (TDRE/RDRF)
 * serializes to one byte in flight -- ~10+ peripheral-register round-trips at
 * ~341 ns each per byte, i.e. ~5 us of wire dead time between 320 ns bytes
 * (scope + bus-probe measured 2026-06-04).  The engine therefore runs the
 * SCI's 32-deep FIFOs (CCR3.FM, armed in configure()) with a credit-bound
 * burst loop sized off the FIFO fill counters, keeping SCK dense; the MASTER
 * paces SCK (the SCI only clocks while the TX FIFO has data), so a late
 * burst merely stretches a gap -- the GD32 slave's DMA capture is
 * indifferent.  Frames are <= 69 bytes, so worst-case CPU time stays tens of
 * microseconds per transaction, with zero per-byte interrupts. */
#define ALP_V2N_POLL_GUARD 100000u /* per-flag spin bound (~ms at 25 MHz) */

/* ── SCI FIFO geometry (RZ/V2N RSCI) ─────────────────────────────────────────
 * Every SCI channel on this SoC has 32-deep TX/RX FIFOs (vendor BSP:
 * BSP_FEATURE_SCI_UART_FIFO_DEPTH = 32, FIFO_CHANNELS = 0x3FF; the FRSR.R /
 * FTSR.T fill counters are 6-bit, FCR triggers 5-bit).  The polled engine
 * caps bytes in flight at DEPTH-1: every outstanding byte is in the TX FIFO,
 * the shifter, or the RX FIFO, so with at most 31 outstanding neither FIFO
 * can overflow -- no per-write TDRE check and no RX overrun, by construction.
 * FCR trigger levels are immaterial to the engine (it batches on the FRSR.R
 * fill count, not RDRF/TDRE) -- written as 0 so the flags keep their plainest
 * meaning (TDRE = TX FIFO empty, RDRF = RX non-empty) for debug reads. */
#define ALP_V2N_SCI_FIFO_DEPTH  32u
#define ALP_V2N_SCI_FIFO_CREDIT (ALP_V2N_SCI_FIFO_DEPTH - 1u)
#define ALP_V2N_SCI_FCR_TRIGGERS                                                                   \
	((0U << R_SCI_B0_FCR_TTRG_Pos) | (0U << R_SCI_B0_FCR_RTRG_Pos))

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

/* RZ-port deviation (silicon 2026-06-06): the rzv2n BSP trigger table marks
 * the RSCI7 RXI/TXI requests DETECTION_RISING_EDGE, but the RSCI sc_rxi/
 * sc_txi DMA requests are LEVEL semantics -- the SAME table's SCIF rows use
 * DETECTION_HIGH_LEVEL with the identical MASK_DACK ack mode, and the
 * activation-source ENABLE macro re-imposes the enum's detection bits on
 * EVERY per-transfer arm (it overwrites CHCFG.HIEN/LVL from the enum, so
 * the e2-studio ext-cfg internal_detection setting is dead code).  An
 * edge-armed channel whose request line is ALREADY ASSERTED at arm time
 * (TDRE = 1 on an idle transmitter) never sees a fresh edge and never
 * moves a beat: with the A55 rz-dmac eliminated via DT (exclusive CM33
 * ownership) every armed transfer timed out beat-less with CHSTAT clean.
 * Override only the detection field; id + ack mode stay per HWM Table
 * 4.7-22. */
#define ALP_V2N_DMAC_DETECTION_FIELD_Msk (7 << 24)
#define ALP_V2N_DMAC_TRIGGER_SCI7_RXI                                                              \
    ((DMAC_TRIGGER_EVENT_SCI7_RXI & ~ALP_V2N_DMAC_DETECTION_FIELD_Msk) | DETECTION_HIGH_LEVEL)
#define ALP_V2N_DMAC_TRIGGER_SCI7_TXI                                                              \
    ((DMAC_TRIGGER_EVENT_SCI7_TXI & ~ALP_V2N_DMAC_DETECTION_FIELD_Msk) | DETECTION_HIGH_LEVEL)
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

/* Per-transaction chip-select.  With the direct latch shim active the shim
 * is THE CS owner (see the block comment at the top): the spi_context GPIO
 * path cannot reach the P97 pad on this silicon, and its per-edge
 * k_busy_wait(cs.delay) calls cost ~14 us of dead time per transaction
 * (k_busy_wait overshoot included; link-bench probe 2026-06-04), so it is
 * skipped entirely rather than stacked on top.  Without the shim, the
 * standard spi_context path applies unchanged. */
static ALWAYS_INLINE void rz_sci_b_spi_cs_set(struct rz_sci_b_spi_data *data, bool assert_cs)
{
#if ALP_V2N_DIRECT_CS_SHIM
	ARG_UNUSED(data);
	if (assert_cs) {
		alp_v2n_cs_assert();
	} else {
		alp_v2n_cs_deassert();
	}
#else
	spi_context_cs_control(&data->ctx, assert_cs);
#endif
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
		rz_sci_b_spi_cs_set(data, false);
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
		rz_sci_b_spi_cs_set(data, false);
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
		rz_sci_b_spi_cs_set(data, false);
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

#if !ALP_V2N_SCI7_DMAC
	/* Arm the SCI's 32-deep FIFOs for the polled engine.  The vendored FSP
	 * gates FIFO mode (CCR3.FM) behind its DMA transfer interfaces
	 * (SCI_B_SPI_CFG_FIFO_SUPPORT + a p_transfer_tx/rx param check), so
	 * its Open leaves FM = 0 here -- but the IP supports FIFO in
	 * Simple-SPI regardless, and the polled engine NEEDS it: without a
	 * FIFO the receiver must drain RDR within one byte-time (320 ns at
	 * 25 MHz) to avoid overrun, which forces a serialized
	 * one-byte-in-flight walk whose register round-trips cost ~5 us of
	 * wire dead time per byte (scope + bus-probe measured 2026-06-04: one
	 * SCI7 register access = ~341 ns).  With the FIFOs absorbing
	 * wire-rate bursts the engine keeps up to 31 bytes in flight and the
	 * wire runs dense (see rz_sci_b_spi_xfer_polled).
	 *
	 * Sequence per the FSP's own FIFO arming (r_sci_b_spi_hw_config): FM
	 * is set while TE/RE are off (Open left CCR0 = 0; the engine toggles
	 * them per transaction), and the FIFO-reset strobes MUST be re-issued
	 * AFTER FM is set.
	 *
	 * DMA mode runs WITHOUT FIFO mode (vendor generator parity: the
	 * e2-studio SCI+DMAC reference leaves FM = 0; the DMAC's per-byte
	 * TDRE/RDRF requests pace the wire instead). */
	{
		R_SCI_B0_Type *p_reg = data->fsp_ctrl.p_reg;

		p_reg->CCR3_b.FM = 1U;
		p_reg->FCR = ALP_V2N_SCI_FCR_TRIGGERS | R_SCI_B0_FCR_TFRST_Msk |
			     R_SCI_B0_FCR_RFRST_Msk;
	}
#endif /* !ALP_V2N_SCI7_DMAC */

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
 *   - flushes both FIFOs (TFRST/RFRST) and clears the receive-data-ready +
 *     error status (RDRFC/ORERC/FERC/PERC); stale bytes left in the RX FIFO
 *     would shift the very next transfer by as many frames (CRC fail ->
 *     STATUS_IO, the symptom this fix kills);
 *   - sequences the TE clear through the FIFO-mode TEND anomaly workaround
 *     (TX-FIFO reset + FM drop first), then re-arms FIFO mode, so a stall
 *     exit cannot wedge the transmitter for the retry that follows;
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

	/* FIFO-mode state is read at runtime so this recovery is correct for
	 * both engines (polled = FM set in configure(); DMA mode runs FM=0). */
	const uint32_t fifo_mode = p_reg->CCR3 & R_SCI_B0_CCR3_FM_Msk;

	/* Stop reception and every transfer-interrupt enable, but keep TE for
	 * a moment: in FIFO mode, clearing TE while TEND = 0 leaves the
	 * transmitter abnormal on its next enable (vendor note in
	 * R_SCI_B_SPI_Close).  Resetting the TX FIFO and dropping CCR3.FM
	 * forces TEND back to 1, after which the TE clear is safe -- the
	 * same order the FSP's own FIFO-mode Close uses. */
	p_reg->CCR0 &= (uint32_t)~(R_SCI_B0_CCR0_RE_Msk | R_SCI_B0_CCR0_TIE_Msk |
				   R_SCI_B0_CCR0_RIE_Msk | R_SCI_B0_CCR0_TEIE_Msk);
	if (fifo_mode) {
		p_reg->FCR = ALP_V2N_SCI_FCR_TRIGGERS | R_SCI_B0_FCR_TFRST_Msk |
			     R_SCI_B0_FCR_RFRST_Msk;
		p_reg->CCR3 &= (uint32_t)~R_SCI_B0_CCR3_FM_Msk;
	}
	p_reg->CCR0 &= (uint32_t)~R_SCI_B0_CCR0_TE_Msk;

	(void)p_reg->RDR;
	p_reg->CFCLR = R_SCI_B0_CFCLR_RDRFC_Msk | R_SCI_B0_CFCLR_ORERC_Msk |
		       R_SCI_B0_CFCLR_FERC_Msk | R_SCI_B0_CFCLR_PERC_Msk;

	/* Wait for the internal communication state to settle after the TE/RE
	 * drop (the FSP does the same CESR wait between CCR0 = 0 and the CCR3
	 * write in hw_config; bounded -- a few TCLK cycles normally). */
	for (uint32_t settle = ALP_V2N_POLL_GUARD; (p_reg->CESR != 0U) && (settle != 0U);
	     settle--) {
	}

	/* Re-arm FIFO mode for the next transfer; the FIFO-reset strobes must
	 * be re-issued after FM is set (same order as configure()). */
	if (fifo_mode) {
		p_reg->CCR3 |= R_SCI_B0_CCR3_FM_Msk;
		p_reg->FCR = ALP_V2N_SCI_FCR_TRIGGERS | R_SCI_B0_FCR_TFRST_Msk |
			     R_SCI_B0_FCR_RFRST_Msk;
	}

	data->fsp_ctrl.count = 0U;
	data->fsp_ctrl.rx_count = 0U;
	data->fsp_ctrl.tx_count = 0U;

	irq_unlock(key);
}

#if !ALP_V2N_SCI7_DMAC
/*
 * Zero-interrupt polled full-duplex engine (the 25 MHz production path; see
 * the data-path note at the top of the file).  Walks the whole spi_context
 * buffer set in FIFO-credit bursts: fill the 32-deep TX FIFO up to the
 * 31-byte in-flight credit, then drain the RX FIFO by its FRSR.R fill count
 * (see the burst-sizing/overrun proof at the loop).  The configure() step
 * has already programmed the bit rate (CCR2), frame format + FIFO mode
 * (CCR3) and FIFO triggers (FCR); this engine only drives TE/RE around the
 * transaction, so rz_sci_b_spi_recover() remains valid for any error exit.
 * Synchronous by design -- a worst-case frame (69 B) is ~22 us of wire time
 * at 25 MHz and the engine's burst overhead adds little on the 200 MHz CM33.
 */
static int rz_sci_b_spi_xfer_polled(struct rz_sci_b_spi_data *data)
{
	R_SCI_B0_Type *p_reg = data->fsp_ctrl.p_reg;
	struct spi_context *ctx = &data->ctx;
	uint32_t guard;

	/* Engage the transceiver.  TE+RE together: the FSP warns RE-without-TE
	 * free-runs SCK, and TE-only would discard the (full-duplex) returns. */
	p_reg->CCR0 |= (R_SCI_B0_CCR0_TE_Msk | R_SCI_B0_CCR0_RE_Msk);

	/* FIFO-credit engine: keep up to (depth - 1) = 31 bytes in flight so
	 * the shifter never starves between bytes -- SCK then runs in dense
	 * bursts instead of one isolated byte every ~5 us (the cost of the
	 * old serialized one-in-flight walk: ~10+ register round-trips at
	 * ~341 ns each per byte).  The credit cap is the whole overrun/
	 * overflow proof: every outstanding byte (sent - recvd) is in the TX
	 * FIFO, the shifter, or the RX FIFO, so with at most 31 outstanding
	 * and both FIFOs 32 deep neither can overflow -- which is also why
	 * there is NO per-write TDRE/room check: the credit bound implies TX
	 * FIFO room.  Bursts are sized once from the FRSR.R fill count (one
	 * ~341 ns status read amortized over the burst) and the data loops
	 * then touch only TDR/RDR, so the per-byte cost approaches the
	 * 320 ns wire time instead of the flag-polled ~5 us.
	 *
	 * TX and RX walk the spi_context independently (sent/recvd) one
	 * contiguous chunk at a time, preserving the existing semantics:
	 * missing/NOP TX buffer clocks 0x00 filler, missing RX buffer
	 * discards (reads still drain the FIFO). */
	const size_t n_tx = spi_context_total_tx_len(ctx);
	const size_t n_rx = spi_context_total_rx_len(ctx);
	const size_t n = MAX(n_tx, n_rx);
	size_t sent = 0;
	size_t recvd = 0;

	guard = ALP_V2N_POLL_GUARD;
	while (recvd < n) {
		/* TX burst: as much as remains, bounded by the in-flight
		 * credit and the current contiguous spi_context chunk. */
		size_t burst = MIN(n - sent, ALP_V2N_SCI_FIFO_CREDIT - (sent - recvd));

		if ((burst > 0) && spi_context_tx_on(ctx)) {
			const uint8_t *src = ctx->tx_buf;

			burst = MIN(burst, ctx->tx_len);
			if (src != NULL) {
				for (size_t i = 0; i < burst; i++) {
					p_reg->TDR = (0xFFFFFF00UL | src[i]);
				}
			} else {
				for (size_t i = 0; i < burst; i++) {
					p_reg->TDR = 0xFFFFFF00UL; /* NOP chunk */
				}
			}
			spi_context_update_tx(ctx, 1, burst);
		} else {
			for (size_t i = 0; i < burst; i++) {
				p_reg->TDR = 0xFFFFFF00UL; /* past TX set: filler */
			}
		}
		sent += burst;

		/* RX burst: drain what the RX FIFO reports, bounded by the
		 * outstanding count and the current contiguous chunk. */
		size_t take = (p_reg->FRSR & R_SCI_B0_FRSR_R_Msk) >> R_SCI_B0_FRSR_R_Pos;

		take = MIN(take, sent - recvd);
		if ((take > 0) && spi_context_rx_on(ctx)) {
			uint8_t *dst = ctx->rx_buf;

			take = MIN(take, ctx->rx_len);
			if (dst != NULL) {
				for (size_t i = 0; i < take; i++) {
					dst[i] = (uint8_t)(p_reg->RDR & 0xFFU);
				}
			} else {
				for (size_t i = 0; i < take; i++) {
					(void)p_reg->RDR; /* NOP chunk */
				}
			}
			spi_context_update_rx(ctx, 1, take);
		} else {
			for (size_t i = 0; i < take; i++) {
				(void)p_reg->RDR; /* past RX set: discard */
			}
		}
		recvd += take;

		if ((burst | take) != 0U) {
			guard = ALP_V2N_POLL_GUARD;
		} else if (--guard == 0U) {
			goto stalled;
		}
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
#endif /* !ALP_V2N_SCI7_DMAC */

#if ALP_V2N_SCI7_DMAC
/*
 * Start one FSP-driven (DMAC) transfer chunk.  Completion arrives through
 * rz_sci_b_spi_callback: SPI_EVENT_TRANSFER_COMPLETE either kicks the next
 * contiguous chunk (rz_sci_b_spi_retransmit) or drops CS and completes the
 * spi_context; error events complete with -EIO.  The synchronous caller
 * parks in spi_context_wait_for_completion().
 */
static int rz_sci_b_spi_xfer_fsp_start(struct rz_sci_b_spi_data *data)
{
	fsp_err_t fsp_err;

	data->data_len = spi_context_max_continuous_chunk(&data->ctx);

	if (data->ctx.rx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Write(&data->fsp_ctrl, data->ctx.tx_buf, data->data_len,
					    SPI_BIT_WIDTH_8_BITS);
	} else if (data->ctx.tx_buf == NULL) {
		fsp_err = R_SCI_B_SPI_Read(&data->fsp_ctrl, data->ctx.rx_buf, data->data_len,
					   SPI_BIT_WIDTH_8_BITS);
	} else {
		fsp_err = R_SCI_B_SPI_WriteRead(&data->fsp_ctrl, data->ctx.tx_buf,
						data->ctx.rx_buf, data->data_len,
						SPI_BIT_WIDTH_8_BITS);
	}

	if (fsp_err != FSP_SUCCESS) {
		LOG_ERR("SCI-SPI FSP transfer start error: %d", fsp_err);
		return -EIO;
	}
	return 0;
}
#endif /* ALP_V2N_SCI7_DMAC */

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
	rz_sci_b_spi_cs_set(data, true);

#if ALP_V2N_SCI7_DMAC
	/* DMAC data path: the FSP streams the chunk; the event callback drops
	 * CS and completes the context (asynchronous callers return here and
	 * get their callback from that path). */
	ret = rz_sci_b_spi_xfer_fsp_start(data);
	if (ret != 0) {
		rz_sci_b_spi_cs_set(data, false);
	} else if (!asynchronous) {
		ret = spi_context_wait_for_completion(&data->ctx);
		if (ret != 0) {
			/* Timeout: the callback never fired -- quiesce the
			 * channel + controller and drop CS ourselves. */
			rz_sci_b_spi_recover(data);
			rz_sci_b_spi_cs_set(data, false);
		}
	}
#else
	ret = rz_sci_b_spi_xfer_polled(data);

	rz_sci_b_spi_cs_set(data, false);

	if (asynchronous) {
		/* The engine is synchronous; satisfy the async contract by
		 * completing immediately (fires the user callback). */
		spi_context_complete(&data->ctx, dev, ret);
	}
#endif /* ALP_V2N_SCI7_DMAC */

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
		/* VENDOR GROUND TRUTH (e2 studio FSP generator, SCI-SPI + DMAC-B                  \
		 * pairing for this SoC family, obtained 2026-06-04): ack_mode =                   \
		 * MASK_DACK_OUTPUT, internal_detection = NO_DETECTION, request                    \
		 * direction = SOURCE_MODULE for BOTH channels (the SCI is the                     \
		 * requesting module on receive too), dreq/ack/tend pins =                         \
		 * NO_INPUT/NO_OUTPUT.  Our 2026-06-03 bring-up sweeps never                       \
		 * tested this exact combination -- each axis was varied from a                    \
		 * different base (BUS_CYCLE ack came from the Linux rz-dmac                       \
		 * convention after MASK_DACK appeared to park SUSPENDED,                          \
		 * HIGH_LEVEL detection from the armed-before-TE edge-consumption                  \
		 * trap, RX had DESTINATION_MODULE, i.e. REQD backwards per the                    \
		 * vendor, and the pin fields were LEFT ZERO-INITIALISED = PIN0,                   \
		 * which makes the FSP program the INTC external-DREQ routing for                  \
		 * pin 0 on top of the internal trigger).  The config below now                    \
		 * mirrors the generator; UNVALIDATED on our silicon -- bench the                  \
		 * gate before shipping it enabled, and only then revisit the                      \
		 * Renesas ticket (its premise changed). */                                        \
		.ack_mode = DMAC_B_ACK_MODE_MASK_DACK_OUTPUT,                                      \
		.dreq_input_pin = DMAC_B_EXTERNAL_INPUT_PIN_NO_INPUT,                              \
		.ack_output_pin = DMAC_B_EXTERNAL_OUTPUT_PIN_NO_OUTPUT,                            \
		.tend_output_pin = DMAC_B_EXTERNAL_OUTPUT_PIN_NO_OUTPUT,                           \
		.external_detection_mode = DMAC_B_EXTERNAL_DETECTION_LOW_LEVEL,                    \
		.internal_detection_mode = DMAC_B_INTERNAL_DETECTION_NO_DETECTION,                 \
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

/* Request direction is SOURCE_MODULE for BOTH directions -- per the
 * vendor generator the on-chip SCI is the requesting module whether it
 * is the transfer source (RX: SCI -> memory) or destination (TX). */
#define RZ_SCI_B_SPI_DMAC_PAIR(n)                                                                  \
	RZ_SCI_B_SPI_DMAC_DEFINE(n, rx, ALP_V2N_SCI7_DMAC_RX_CH,                                   \
				 ALP_V2N_DMAC_TRIGGER_SCI7_RXI,                                    \
				 DMAC_B_REQUEST_DIRECTION_SOURCE_MODULE,                           \
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
