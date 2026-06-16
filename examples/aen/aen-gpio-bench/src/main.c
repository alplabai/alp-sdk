/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon GPIO validation for the E1M-AEN801 (Alif Ensemble E8) over the
 * UPSTREAM DesignWare gpio_dw driver (ADR 0017 Tier-1, "snps,designware-gpio").
 *
 * What it proves
 * --------------
 * Drive a SAFE, uncontended pin -- P8_0 (gpio8, pin 0) -- as a push-pull
 * output, set it HIGH then LOW through the portable Zephyr GPIO API, and after
 * each step read back the DesignWare data (DR) + direction (DDR) registers so
 * the result is verifiable two independent ways:
 *
 *   1. in-firmware:  this app reads DR/DDR/EXT_PORTA via sys_read32() and prints
 *      them, plus every gpio_pin_configure/gpio_pin_set return code, and a
 *      single 'RESULT PASS:' / 'RESULT FAIL:' line.
 *   2. over J-Link:  the human reads the same registers with mem32 (addresses
 *      below) -- the ground truth on silicon, independent of any printk.
 *
 * DesignWare gpio register map for gpio8 @ 0x49008000 (PORT-A; gpio_dw derives
 * port 0 because 0x49008000 & 0x3f == 0):
 *     DR  (SWPORTA_DR)  = 0x49008000    bit0 = driven output level
 *     DDR (SWPORTA_DDR) = 0x49008004    bit0 = 1 -> output
 *     EXT_PORTA         = 0x49008050    bit0 = read-back pad level
 *
 * Expected (pin = bit 0):
 *     after configure OUTPUT_HIGH / set 1:  DDR bit0 = 1, DR bit0 = 1
 *     after set 0:                          DDR bit0 = 1, DR bit0 = 0
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/pinctrl/alif-ensemble-pinctrl.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/* gpio8 node + the pad-mux state attached in the overlay. */
#define GPIO8_NODE   DT_NODELABEL(gpio8)
#define TEST_PIN     0U                     /* P8_0 -> DR/DDR bit 0 */
#define TEST_BIT     BIT(TEST_PIN)

/* DesignWare PORT-A register absolute addresses for this node. These match
 * what gpio_dw writes; we read them directly purely to self-verify. */
#define GPIO8_BASE       DT_REG_ADDR(GPIO8_NODE)   /* 0x49008000 */
#define DW_SWPORTA_DR    (GPIO8_BASE + 0x00U)      /* 0x49008000 */
#define DW_SWPORTA_DDR   (GPIO8_BASE + 0x04U)      /* 0x49008004 */
#define DW_EXT_PORTA     (GPIO8_BASE + 0x50U)      /* 0x49008050 */

/* P8_0 -> GPIO (AF0).  gpio_dw does not touch the pad mux, and the upstream
 * "snps,designware-gpio" binding carries no pinctrl-0, so apply the mux straight
 * through the Alif pinctrl SoC driver (pinctrl_soc_pin_t is the uint32_t PINMUX
 * word; the Alif impl ignores the reg argument). */
static const pinctrl_soc_pin_t p8_0_gpio[] = { PIN_P8_0__GPIO };

static const struct device *const gpio8 = DEVICE_DT_GET(GPIO8_NODE);

static void dump_regs(const char *tag)
{
	uint32_t dr  = sys_read32(DW_SWPORTA_DR);
	uint32_t ddr = sys_read32(DW_SWPORTA_DDR);
	uint32_t ext = sys_read32(DW_EXT_PORTA);

	printk("  [%s] DR=0x%08x (bit0=%u) DDR=0x%08x (bit0=%u) EXT=0x%08x (bit0=%u)\n",
	       tag, dr, (unsigned int)((dr >> TEST_PIN) & 1U),
	       ddr, (unsigned int)((ddr >> TEST_PIN) & 1U),
	       ext, (unsigned int)((ext >> TEST_PIN) & 1U));
}

int main(void)
{
	int rc;
	uint32_t dr_hi, ddr_hi, dr_lo, ddr_lo;

	printk("\n=== AEN801 GPIO bench (gpio_dw / P8_0 / gpio8 pin %u) ===\n",
	       TEST_PIN);
	printk("base=0x%08x  DR=0x%08x  DDR=0x%08x  EXT=0x%08x\n",
	       (unsigned int)GPIO8_BASE, (unsigned int)DW_SWPORTA_DR,
	       (unsigned int)DW_SWPORTA_DDR, (unsigned int)DW_EXT_PORTA);

	/* 1. mux P8_0 -> GPIO (gpio_dw never touches the pad mux). */
	rc = pinctrl_configure_pins(p8_0_gpio, ARRAY_SIZE(p8_0_gpio), 0U);
	printk("pinctrl_configure_pins(P8_0->GPIO) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: pinctrl_configure_pins rc=%d\n", rc);
		return 0;
	}

	/* 2. device readiness. */
	if (!device_is_ready(gpio8)) {
		printk("RESULT FAIL: gpio8 device not ready\n");
		return 0;
	}
	printk("gpio8 device ready\n");
	dump_regs("pre-config");

	/* 3. configure as output, init HIGH. */
	rc = gpio_pin_configure(gpio8, TEST_PIN, GPIO_OUTPUT_HIGH);
	printk("gpio_pin_configure(OUTPUT_HIGH) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: gpio_pin_configure rc=%d\n", rc);
		return 0;
	}
	dump_regs("after-config-high");

	/* 4. explicit drive HIGH. */
	rc = gpio_pin_set(gpio8, TEST_PIN, 1);
	printk("gpio_pin_set(1) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: gpio_pin_set(1) rc=%d\n", rc);
		return 0;
	}
	k_busy_wait(1000); /* let the level settle for the J-Link mem32 read */
	dump_regs("driven-high");
	dr_hi  = sys_read32(DW_SWPORTA_DR);
	ddr_hi = sys_read32(DW_SWPORTA_DDR);

	/* --- J-Link readback window #1 (HIGH): mem32 0x49008000 / 0x49008004 --- */

	/* 5. drive LOW. */
	rc = gpio_pin_set(gpio8, TEST_PIN, 0);
	printk("gpio_pin_set(0) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: gpio_pin_set(0) rc=%d\n", rc);
		return 0;
	}
	k_busy_wait(1000);
	dump_regs("driven-low");
	dr_lo  = sys_read32(DW_SWPORTA_DR);
	ddr_lo = sys_read32(DW_SWPORTA_DDR);

	/* --- J-Link readback window #2 (LOW): mem32 0x49008000 / 0x49008004 --- */

	/* 6. verdict: DDR shows the pin as output in BOTH states, and DR tracks
	 *    the driven level (high bit set, low bit clear). */
	bool ddr_ok = ((ddr_hi & TEST_BIT) != 0U) && ((ddr_lo & TEST_BIT) != 0U);
	bool dr_ok  = ((dr_hi  & TEST_BIT) != 0U) && ((dr_lo  & TEST_BIT) == 0U);

	if (ddr_ok && dr_ok) {
		printk("RESULT PASS: gpio_dw P8_0 output drives high then low "
		       "(DDR bit0=1, DR high=1 low=0)\n");
	} else {
		printk("RESULT FAIL: ddr_ok=%d dr_ok=%d "
		       "DDR_hi=0x%08x DDR_lo=0x%08x DR_hi=0x%08x DR_lo=0x%08x\n",
		       (int)ddr_ok, (int)dr_ok, ddr_hi, ddr_lo, dr_hi, dr_lo);
	}

	/* Leave the pin LOW + park; the registers stay readable over J-Link. */
	return 0;
}
