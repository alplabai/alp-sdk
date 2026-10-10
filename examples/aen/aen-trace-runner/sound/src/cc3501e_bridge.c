/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM CC3501E bridge bring-up helper -- see cc3501e_bridge.h.
 * Copied from alp-sdk examples/aen/aen-evk-demo/src/cc3501e_bridge.c; the
 * one change: the LP-pad output enable below also runs on the M55-HP
 * (sound/ runs there in the game), not only on the HE.  The warm-aware final
 * reset (no WIFI_EN cycle on a running CC3501E, issue #2797) matches the
 * canonical copy.
 */

#include "cc3501e_bridge.h"

#if defined(TR_SND_EMBED) && TR_SND_EMBED
/* Combined HP image (sound/src/main.c): the proxy attach below reads the identity EEPROM @0x50
 * on I2C2, which the HE may own -- lease the bus for it (Dekker entry, src/ipc/tr_bus2.h) and
 * GIVE IT BACK right after, before the long, bus-free CC3501E reset: this core's I2C2 IRQ is off
 * and the HE owns the bus again for that whole stretch. A take-back seen at the entry aborts the
 * bring-up (main.c reads its s_aborted); a retry reuses the control-pin and SPI handles opened
 * here. */
#include <stdbool.h>
bool tr_snd_bus_enter(void);
void tr_snd_bus_release(void);
#define BRIDGE_EMBED 1
static alp_gpio_t *s_wifi_en, *s_nrst;
static alp_spi_t  *s_spi;
#else
#define BRIDGE_EMBED 0
#endif

#if defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HE) || defined(CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP)
#include <zephyr/arch/cpu.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/sys_io.h>
/*
 * AEN LP-pad mux (Alif Ensemble E8, M55-HE).  WIFI_EN (P15_5) and nRESET (P15_1)
 * are on the Alif LP-GPIO island, bound by the generic snps,designware-gpio driver
 * which does NOT apply Alif pinctrl -- so the LP pads stay un-muxed and their output
 * drivers OFF (confirmed on silicon: WIFI_EN never powers the CC3501E until these
 * regs are set).  0x23 = the Alif GPIO-output pad config (driver + read-enable +
 * drive strength).  TODO: drop this raw poke once the Alif GPIO backend muxes the LP
 * island via pinctrl.
 */
#define ALIF_LPGPIO_PADCTRL_BASE 0x42007000u
#define ALIF_PAD_GPIO_OUTPUT     0x23u
static void aen_lp_pads_enable_output(void)
{
	sys_write32(ALIF_PAD_GPIO_OUTPUT, ALIF_LPGPIO_PADCTRL_BASE + 5u * 4u); /* P15_5 WIFI_EN */
	sys_write32(ALIF_PAD_GPIO_OUTPUT, ALIF_LPGPIO_PADCTRL_BASE + 1u * 4u); /* P15_1 nRESET  */
}

/* Latch WIFI_EN (P15_5) HIGH in the port's data register while the pad is still an
 * INPUT.  Switching the direction afterwards then drives the already-high level; the
 * other order (direction first, value second -- what gpio_dw's configure does even
 * with an init flag) drives whatever DR holds, and with DR bit 5 = 0 that glitches
 * the supply of a running chip low.  gpio_port_set_bits_raw() writes DR regardless
 * of direction. */
static void aen_wifi_en_latch_high(void)
{
	const struct device *lpgpio = DEVICE_DT_GET(DT_NODELABEL(lpgpio));

	if (device_is_ready(lpgpio)) {
		(void)gpio_port_set_bits_raw(lpgpio, BIT(5));
	}
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
#if BRIDGE_EMBED
	alp_gpio_t *wifi_en = s_wifi_en != NULL ? s_wifi_en : alp_gpio_open(CC3501E_BRIDGE_PIN_WIFI_EN);
	alp_gpio_t *nrst    = s_nrst != NULL ? s_nrst : alp_gpio_open(CC3501E_BRIDGE_PIN_NRST);
	s_wifi_en           = wifi_en;
	s_nrst              = nrst;
#else
	alp_gpio_t *wifi_en = alp_gpio_open(CC3501E_BRIDGE_PIN_WIFI_EN);
	alp_gpio_t *nrst    = alp_gpio_open(CC3501E_BRIDGE_PIN_NRST);
#endif
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
#if BRIDGE_EMBED
	alp_spi_t *spi = s_spi != NULL ? s_spi : alp_spi_open(&(alp_spi_config_t) {
#else
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
#endif
		.bus_id = CC3501E_BRIDGE_SPI_BUS_ID, .freq_hz = CC3501E_BRIDGE_SPI_FREQ_HZ,
		.mode = ALP_SPI_MODE_0, .bits_per_word = 8u, .cs_pin_id = ALP_SPI_NO_CS,
	});
#if BRIDGE_EMBED
	s_spi = spi;
#endif
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
#if BRIDGE_EMBED
	if (!tr_snd_bus_enter()) {
		return ALP_ERR_BUSY; /* the HE took I2C2 back: no EEPROM read */
	}
#endif
	(void)alp_gpio_cc3501e_attach(fw); /* reads the identity EEPROM @0x50 on I2C2 */
#if BRIDGE_EMBED
	tr_snd_bus_release();
#endif
#endif
#ifdef CONFIG_ALP_SDK_WIFI_CC3501E
	(void)alp_wifi_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_BLE_CC3501E
	(void)alp_ble_cc3501e_attach(fw);
#endif
	if (!wifi_was_high) {
		return cc3501e_reset(fw); /* supply low: the full cold-boot sequence */
	}
	/* WIFI_EN reads high.  Warm = the chip ANSWERS a PING; a pull-up alone proves
	 * nothing.  Warm: nRESET only, never a WIFI_EN toggle. */
	if (cc3501e_ping(fw) == ALP_OK) {
		return cc3501e_hard_reset(fw);
	}
	/* High but silent: powered-and-hung, or unpowered behind a pull-up.  Try the
	 * nRESET-only reset first (safe for a powered chip); only if the chip still does
	 * not answer is the supply cycled -- the last resort, taken from a chip that is
	 * already unresponsive. */
	if (cc3501e_hard_reset(fw) == ALP_OK && cc3501e_ping(fw) == ALP_OK) {
		return ALP_OK;
	}
	return cc3501e_reset(fw);
}
