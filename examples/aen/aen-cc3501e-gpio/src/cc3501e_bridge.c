/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * E1M-AEN SoM CC3501E bridge bring-up helper -- see cc3501e_bridge.h.
 */

#include "cc3501e_bridge.h"

#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HE)
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
	 * silicon-tuned -- sweep CC3501E_BRIDGE_RX_SAMPLE_DLY at the target SCLK. */
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
	/* Optional host-IRQ/READY line (r2 SS0+IRQ bridge): CC35 GPIO17 -> Alif P2_6.
	 * Open + configure as input; if the board doesn't wire it (alp_pins[2] absent),
	 * alp_gpio_open returns NULL and the driver keeps the fixed-gap fallback. */
	alp_gpio_t *ready = alp_gpio_open(CC3501E_BRIDGE_PIN_READY);
	if (ready != NULL) {
		(void)alp_gpio_configure(ready, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);
		fw->ready_pin = ready;
	}
#ifdef CONFIG_ALP_SDK_GPIO_CC3501E_PROXY
	(void)alp_gpio_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_WIFI_CC3501E
	(void)alp_wifi_cc3501e_attach(fw);
#endif
#ifdef CONFIG_ALP_SDK_BLE_CC3501E
	(void)alp_ble_cc3501e_attach(fw);
#endif
	return cc3501e_reset(fw);
}
