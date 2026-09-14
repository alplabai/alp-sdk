/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM CC3501E bridge bring-up helper -- see cc3501e_bridge.h.
 */

#include "cc3501e_bridge.h"

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HE)
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
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

/* ---- chip-select wiring --
 * CS = the dwc-ssi HARDWARE SS0 (Alif P14_7 muxed as SPI1_SS0_C in pinctrl_spi1):
 *      the SPI peripheral asserts/deasserts it per transfer (no software GPIO CS,
 *      not CS-less).  The host opens ALP_SPI_NO_CS so the alp_spi backend leaves
 *      cs_present=false and spi_dw takes the hardware SER/SS0 branch.  No CS hooks
 *      needed here: the host driver does not bracket transactions with any
 *      software chip-select.
 *
 * READY (CC35 GPIO17) is NOT wired here: fw->ready_pin stays NULL, same as every
 * other AEN example bridge by default -- see chips/cc3501e/cc3501e_core.c's
 * cc3501e_reply_gate() comment for the pin-routing fact and bench evidence.  This
 * file used to mux + input-enable Alif P2_6 (gpio2.6) as a bodge under the belief
 * it carried READY; it does not (P2_6 is E1M pad AH7 / I2S1_SCLK, a DIFFERENT net
 * from CC35 GPIO17 / E1M pad G3), and nothing here ever read it back, so that
 * pinctrl mux was pure dead weight and has been removed along with it. */
#else
static inline void aen_lp_pads_enable_output(void)
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
	(void)alp_gpio_configure(wifi_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	(void)alp_gpio_configure(nrst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

	/* 2. Inter-chip SPI (Alif = master).  cs_pin_id = ALP_SPI_NO_CS so the alp_spi
	 *    backend leaves cs_present=false and spi_dw drives the dwc-ssi HARDWARE SS0
	 *    (P14_7 = SPI1_SS0_C from pinctrl) per transfer -- a peripheral-driven chip-
	 *    select, NOT a software GPIO CS and NOT CS-less.  Mode 0 matches the CC3501E
	 *    vendor image frameFormat. */
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

	/* 3. Bind the bus + control pins; attach the GPIO proxy (proxied E1M IOs then
	 *    route over the bridge); run the power + reset sequence (TI SWRU626 + the
	 *    Puya cold-boot hard-reset workaround).  Leaves WIFI_EN HIGH. */
	(void)cc3501e_init(fw, spi);
	fw->enable_pin = wifi_en;
	fw->reset_pin  = nrst;
#ifdef CONFIG_ALP_SDK_GPIO_CC3501E_PROXY
	(void)alp_gpio_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_WIFI_CC3501E
	(void)alp_wifi_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_BLE_CC3501E
	(void)alp_ble_cc3501e_attach(fw);
#endif
	/* Power + reset sequence (cold-cycle + Puya hard-reset workaround).  A
	 * TRANSPORT-level non-OK from cc3501e_reset() here is NOT fatal: a cold
	 * CC35 commonly mis-reads on first contact and only aligns after the
	 * hard-reset soak below, so fall through rather than abort -- otherwise
	 * the caller never registers the companion on a cold boot (bench-
	 * confirmed on the E1M-AEN801: the early return left the shell reporting
	 * "companion not registered" until the soak ran).
	 *
	 * ALP_ERR_VERSION is the one exception (#1371): it means cc3501e_reset()'s
	 * own GET_VERSION probe DID land and the firmware DID answer -- with a
	 * protocol version this host refuses to talk to.  That is permanent, not
	 * transient (retrying cannot reconcile two binaries that disagree about
	 * the wire), so unlike a transport hiccup it must not be swallowed into
	 * the retry soak below: surface it so the caller leaves the companion
	 * unregistered instead of silently talking a frame layout the firmware
	 * never claimed to speak. */
	alp_status_t reset_status = cc3501e_reset(fw);
	if (reset_status == ALP_ERR_VERSION) {
		return ALP_ERR_VERSION;
	}
	/* COLD-BOOT SOAK (the cold-boot workaround is a MUST): the Puya double-boot can need
	 * SEVERAL hard resets before the cold-booted image services the bridge.  Retry
	 * cc3501e_hard_reset until a GET_VERSION probe succeeds (cold first-contact aligns via
	 * the CS toggle).  Leaves the handle bound even if it never aligns. */
	for (unsigned i = 0; i < 8u; i++) {
		uint16_t ver = 0;
		if (cc3501e_get_version(fw, &ver) == ALP_OK) {
			return ALP_OK;
		}
		(void)cc3501e_hard_reset(fw);
	}
	return ALP_OK;
}
