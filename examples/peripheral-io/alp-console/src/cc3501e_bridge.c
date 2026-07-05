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
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
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

/* ---- chip-select + READY/host-IRQ wiring --
 * CS  = the dwc-ssi HARDWARE SS0 (Alif P14_7 muxed as SPI1_SS0_C in pinctrl_spi1):
 *       the SPI peripheral asserts/deasserts it per transfer (no software GPIO CS,
 *       not CS-less).  The host opens ALP_SPI_NO_CS so the alp_spi backend leaves
 *       cs_present=false and spi_dw takes the hardware SER/SS0 branch.
 * READY = CC35 GPIO17 (E1M IO16) -> Alif P2_6 (gpio2.6): HIGH = bridge ready to
 *       clock, LOW = mid-radio-op (SPI-slave DMA dead).  Gates cc3501e_request.
 *       This READY line is a rev-1 EVK wire, read as a GPIO input below.
 */
/* READY is a bodge GPIO input (P2_6 = gpio2.6); guard on gpio2 (still in the overlay).
 * CS needs no node here -- it is driven by the SPI peripheral's hardware SS0. */
#if DT_NODE_EXISTS(DT_NODELABEL(gpio2))
/* Mux P2_6 (READY) to GPIO (func 0) with REN (read-enable, pad-config bit 16).  The snps
 * GPIO driver does not apply Alif pinctrl, so do it directly via pinctrl_configure_pins. */
#define ALIF_PAD_REN (1u << 16)
static const pinctrl_soc_pin_t bodge_pins[] = {
    (pinctrl_soc_pin_t)(PIN_P2_6__GPIO | ALIF_PAD_REN) /* READY in, read-enable */
};

static void aen_bodge_init(void)
{
	(void)pinctrl_configure_pins(bodge_pins, ARRAY_SIZE(bodge_pins), PINCTRL_REG_NONE);
	/* READY (P2_6 = gpio2.6) is read via the RAW DesignWare GPIO input register
	 * (EXT_PORTA), bypassing the flaky snps gpio2 driver init (its 8 IRQs).  The
	 * pad mux (GPIO + read-enable) was already applied by pinctrl_configure_pins
	 * above, so the input register reflects the pad. */
}

#define ALIF_GPIO2_BASE      0x49002000u
#define DW_GPIO_EXT_PORTA    0x50u
#define CC3501E_READY_PIN    6u

static int bodge_ready_raw(void)
{
	uint32_t v = sys_read32(ALIF_GPIO2_BASE + DW_GPIO_EXT_PORTA);
	return (v >> CC3501E_READY_PIN) & 1u;
}

/* Strong override of the generic weak cc3501e_bus_ready() (chips/cc3501e.c).
 * Reads the CC35 READY/host-IRQ line (GPIO17 -> P2_6) via the raw DW input
 * register: HIGH = bridge ready to clock, LOW = mid-radio-op (DMA dead, don't
 * clock).  Returning false makes cc3501e_request answer BUSY -> poll_by_repeat
 * retries until READY (no clocking into a dead slave -> no desync). */
bool cc3501e_bus_ready(void)
{
	/* The DW EXT_PORTA first read after an idle gap can return a stale/pre-sync
	 * value (observed "0 1 1"); discard it and use the settled read. */
	(void)bodge_ready_raw();
	return bodge_ready_raw() == 1;
}
/* No CS hooks here: CS is the dwc-ssi hardware SS0 (peripheral-driven per transfer); the
 * host driver does not bracket transactions with any software chip-select. */
#else
static inline void aen_bodge_init(void)
{
}
#endif /* gpio2 (READY) node exists */
#else
static inline void aen_lp_pads_enable_output(void)
{
}
static inline void aen_bodge_init(void)
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

	/* Arm the rev-1 EVK READY/host-IRQ input (P2_6) BEFORE any SPI transaction, so
	 * cc3501e_request can gate on READY through the reset/ping handshake below.  CS is
	 * not touched here -- it is the dwc-ssi hardware SS0, driven by the SPI peripheral
	 * per transfer. */
	aen_bodge_init();

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
	/* Power + reset sequence (cold-cycle + Puya hard-reset workaround). */
	alp_status_t rst = cc3501e_reset(fw);
	if (rst != ALP_OK) {
		return rst;
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
