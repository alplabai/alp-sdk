/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM CC3501E bridge bring-up helper -- see cc3501e_bridge.h.
 *
 * The final reset is warm-aware.  cc3501e_reset() drops WIFI_EN for 50 ms, a
 * supply cold cycle; on a CC3501E that is already running (an activated unit with
 * vendor_sbl_container_enable=1 may then never relaunch) that must not happen on a
 * mere re-run or warm reset of the application.  So when WIFI_EN already reads high
 * AND the chip answers a PING the bring-up resets through nRESET only
 * (cc3501e_hard_reset()), and uses cc3501e_reset() only from WIFI_EN low or, as a
 * last resort, from a chip that stays silent through an nRESET-only reset.  WIFI_EN
 * reading high alone is not trusted: a pull-up would make every power-on look warm
 * (whether the module has one is an open question, issue #2797).
 */

#include "cc3501e_bridge.h"

#include <stdio.h>

#if defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HE) || defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP)
#include <zephyr/arch/cpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/sys_io.h>
/*
 * AEN LP-pad mux (Alif Ensemble E8, both M55-HE and M55-HP cores).  WIFI_EN
 * (P15_5) and nRESET (P15_1) are on the Alif LP-GPIO island, bound by the
 * generic snps,designware-gpio driver which does NOT apply Alif pinctrl --
 * so the LP pads stay un-muxed and their output drivers OFF (confirmed on
 * silicon on M55-HE: WIFI_EN never powers the CC3501E until these regs are
 * set).  0x23 = the Alif GPIO-output pad config (driver + read-enable +
 * drive strength).  TODO: drop this raw poke once the Alif GPIO backend
 * muxes the LP island via pinctrl.
 *
 * ALIF_LPGPIO_PADCTRL_BASE (0x42007000) is not an M55-HE-local address: the
 * shared upstream `pin-controller@1a603000` node (ensemble_common.dtsi,
 * included by both cores' SoC dtsi) declares this exact window as its
 * second `reg` range, and `alp_e1m_aen801_m55_hp-pinctrl.dtsi` already
 * muxes a different LP-GPIO pad (P15_0, RTC_ALARM) from the M55-HP pinctrl
 * driver through it, so the poke below is equally applicable on M55-HP.
 * Running it on M55-HP is NOT bench-proven -- only running it on M55-HE has
 * been confirmed on silicon.
 */
#define ALIF_LPGPIO_PADCTRL_BASE 0x42007000u
#define ALIF_PAD_GPIO_OUTPUT     0x23u
/* Latch WIFI_EN (P15_5) HIGH in the port's data register while the pad is still an
 * INPUT.  Switching the direction afterwards then drives the already-high level; the
 * other order (direction first, value second -- what gpio_dw's configure does even
 * with an init flag) drives whatever DR holds, and with DR bit 5 = 0 that glitches
 * the supply of a running chip low.  DR is written directly (read-modify-write, the
 * same access gpio_dw's port_set_bits_raw makes) so this template stays on the
 * portable <alp/*.h> surface: SWPORTA_DR is offset 0x00 of the DesignWare GPIO block
 * (zephyr drivers/gpio/gpio_dw_registers.h), the block base is the `lpgpio` node's
 * reg, and the LP-GPIO clock is already on because alp_gpio_open() opened the pin. */
#define DW_GPIO_SWPORTA_DR 0x00u
static void aen_wifi_en_latch_high(void)
{
	const mem_addr_t dr = DT_REG_ADDR(DT_NODELABEL(lpgpio)) + DW_GPIO_SWPORTA_DR;

	sys_write32(sys_read32(dr) | (1u << 5), dr);
}

static void aen_lp_pads_enable_output(void)
{
	sys_write32(ALIF_PAD_GPIO_OUTPUT, ALIF_LPGPIO_PADCTRL_BASE + 5u * 4u); /* P15_5 WIFI_EN */
	sys_write32(ALIF_PAD_GPIO_OUTPUT, ALIF_LPGPIO_PADCTRL_BASE + 1u * 4u); /* P15_1 nRESET  */
}
#else
static inline void aen_lp_pads_enable_output(void)
{
}
static inline void aen_wifi_en_latch_high(void)
{
}
#endif

alp_status_t cc3501e_bridge_bringup(cc3501e_t *fw)
{
	if (fw == NULL) {
		return ALP_ERR_INVAL;
	}

	/* 1. Control pins: WIFI_EN (supply gate) + nRESET.  These are Alif LP-GPIO
	 *    pads, not E1M edge pads -- the SoM owns them. */
	alp_gpio_t *wifi_en = alp_gpio_open(CC3501E_BRIDGE_PIN_WIFI_EN);
	alp_gpio_t *nrst    = alp_gpio_open(CC3501E_BRIDGE_PIN_NRST);
	if (wifi_en == NULL || nrst == NULL) {
		if (wifi_en != NULL) {
			alp_gpio_close(wifi_en);
		}
		if (nrst != NULL) {
			alp_gpio_close(nrst);
		}
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	aen_lp_pads_enable_output();
	/* Sample the supply gate BEFORE touching its direction.  High is only a HINT
	 * that the chip may be powered -- an external pull-up on WIFI_EN would make every
	 * power-on read high (whether the module has one is an OPEN QUESTION; the
	 * netlist has not been checked), so the decision below also needs a PING. */
	bool wifi_was_high = false;
	(void)alp_gpio_read(wifi_en, &wifi_was_high);
	if (wifi_was_high) {
		/* Latch the level in DR first so the direction change cannot glitch it low. */
		aen_wifi_en_latch_high();
	}
	(void)alp_gpio_configure(wifi_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	(void)alp_gpio_configure(nrst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

	/* 2. Inter-chip SPI (Alif = master).  CS is the dwc-ssi hardware SS0
	 *    muxed on P14_7; the app passes ALP_SPI_NO_CS so no software GPIO CS
	 *    is installed and the SPI controller drives SS0 per transfer.  Mode 0
	 *    matches the CC3501E vendor image frameFormat. */
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = CC3501E_BRIDGE_SPI_BUS_ID,
	    .freq_hz       = CC3501E_BRIDGE_SPI_FREQ_HZ,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8u,
	    .cs_pin_id     = ALP_SPI_NO_CS,
	});
	if (spi == NULL) {
		alp_gpio_close(wifi_en);
		alp_gpio_close(nrst);
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}

#if CC3501E_BRIDGE_RX_SAMPLE_DLY > 0
	/* Raise the SCLK past 1 MHz: the DW SSI RX_SAMPLE_DLY register (0xf0) delays
	 * the MISO capture point by N ssi_clk cycles to cover the on-SoM trace + crossed
	 * data round-trip.  Zephyr spi_dw never writes it (leaves 0), so without this
	 * >1 MHz mis-samples MISO (cold reqhdr_rx=0xFFFFFFFF).  Written with SSI disabled
	 * (SSIENR=0); persists across the driver's per-transfer configure.  Value is
	 * silicon-tuned -- sweep CC3501E_BRIDGE_RX_SAMPLE_DLY at the target SCLK.
	 *
	 * SWEEP TRAP -- read before setting the constant to 0: the `#if > 0` above
	 * compiles this block out, so 0 does not mean "no delay", it means the SPI
	 * node's `rx-delay` devicetree value is left in place.  aen-cc3501e-bringup
	 * and aen-evk-demo declare rx-delay = <2>, so a naive N=0 sweep point on
	 * those two silently measures 2.  Read 0x481040F0 back to see what the link
	 * is actually running at. */
	{
		volatile uint32_t *ssienr =
		    (volatile uint32_t *)(uintptr_t)(CC3501E_BRIDGE_SPI1_BASE + 0x08u);
		volatile uint32_t *rsd = (volatile uint32_t *)(uintptr_t)(CC3501E_BRIDGE_SPI1_BASE + 0xf0u);
		uint32_t           en  = *ssienr;
		*ssienr                = 0u;
		*rsd                   = (uint32_t)CC3501E_BRIDGE_RX_SAMPLE_DLY;
		*ssienr                = en;
	}
#endif

	/* 3. Bind the bus + control pins; attach the GPIO proxy (proxied E1M IOs then
	 *    route over the bridge); run the power + reset sequence (TI SWRU626 + the
	 *    Puya cold-boot hard-reset workaround).  Leaves WIFI_EN HIGH. */
	(void)cc3501e_init(fw, spi);
	fw->enable_pin = wifi_en;
	fw->reset_pin  = nrst;
	/* READY (CC35 GPIO17) is NOT wired here by default on this R2 module --
	 * see chips/cc3501e/cc3501e_core.c's cc3501e_reply_gate() comment for the
	 * pin-routing fact, the bench evidence, and how a board that genuinely
	 * wires it can opt back in via fw->ready_pin. */
#ifdef CONFIG_ALP_SDK_GPIO_CC3501E_PROXY
	(void)alp_gpio_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_WIFI_CC3501E
	(void)alp_wifi_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_BLE_CC3501E
	(void)alp_ble_cc3501e_attach(fw);
#endif
	/* One line naming the path taken, so a log shows whether the supply was cycled. */
	if (!wifi_was_high) {
		printf("[cc3501e_bridge] WIFI_EN low: cold power-up (cc3501e_reset)\n");
		return cc3501e_reset(fw); /* supply low: the full cold-boot sequence */
	}
	/* WIFI_EN reads high.  Warm = the chip ANSWERS a PING; a pull-up alone proves
	 * nothing.  Warm: nRESET only, never a WIFI_EN toggle. */
	if (cc3501e_ping(fw) == ALP_OK) {
		printf("[cc3501e_bridge] WIFI_EN high, PING ok: warm, nRESET only\n");
		return cc3501e_hard_reset(fw);
	}
	/* High but silent: powered-and-hung, or unpowered behind a pull-up.  Try the
	 * nRESET-only reset first (safe for a powered chip); only if the chip still does
	 * not answer is the supply cycled -- the last resort, taken from a chip that is
	 * already unresponsive. */
	if (cc3501e_hard_reset(fw) == ALP_OK && cc3501e_ping(fw) == ALP_OK) {
		printf("[cc3501e_bridge] WIFI_EN high, silent: recovered by nRESET only\n");
		return ALP_OK;
	}
	printf("[cc3501e_bridge] WIFI_EN high, silent after nRESET: supply cycle (cc3501e_reset)\n");
	return cc3501e_reset(fw);
}
