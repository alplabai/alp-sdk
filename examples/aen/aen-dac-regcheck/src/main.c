/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-dac-regcheck -- scopeless on-silicon validation of the Alif DAC driver
 * (zephyr/drivers/dac/dac_alif.c, compatible "alif,dac", dac0 @ 0x49028000) on
 * the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console
 * flow.
 *
 * We cannot see the analog output pad on this bench (no scope, app UART not on
 * USB, and the dac0 pad P2_2 is not scoped on this batch).  So we validate by
 * driving the standard Zephyr DAC API (dac_channel_setup + dac_write_value, a
 * mid-scale code) and then doing a REGISTER READBACK of the controller's key
 * registers.
 *
 * Two independent confirmations, exactly like aen-adc-regcheck:
 *   1. This firmware drives the API, prints the registers it reads back, plus a
 *      single RESULT PASS/FAIL line, to the RAM console (read 'ram_console_buf'
 *      over J-Link mem8, ASCII-decode).
 *   2. The human re-reads the SAME absolute addresses over J-Link mem32 (see the
 *      readback plan) -- so a driver that only PRINTS the right thing is caught.
 *
 * WRITE path (the bit the strict PASS gate cares about), from
 * dac_alif.c::dac_init() + dac_enable() + dac_write_data():
 *   - dac_init():    dac_reset_deassert() sets DAC_REG1 (0x00) bit27 RESET_B,
 *                    then dac_set_config() programs the trim bits.
 *   - dac_channel_setup() -> dac_enable(): sets DAC_REG1 (0x00) bit0 EN.
 *   - dac_write_value() -> dac_write_data(): writes the code to DAC_IN (0x04)
 *     (input-mux/bypass is OFF by default, so the code lands directly in DAC_IN).
 * Unlike the ADC, the DAC has NO completion teardown and NO IRQ -- the EN +
 * RESET_B bits stay set and DAC_IN retains the written code, so the readback is
 * a direct check that the driver programmed the IP.
 *
 * The analog pad routed to the DAC output is a BENCH UNKNOWN / unscoped on this
 * batch, so there is no measured-voltage check; the PASS gate is purely "the
 * driver programmed the registers": dac_channel_setup() and dac_write_value()
 * both returned 0, AND DAC_IN holds the written code, AND DAC_REG1 has EN +
 * RESET_B set.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/sys/printk.h>

#define DAC_NODE DT_NODELABEL(dac0)

/* Absolute reg base straight from the dtsi reg[0] = "dac_reg" (0x49028000),
 * pulled from devicetree so this stays correct if the node ever moves. */
#define DAC_BASE ((uint32_t)DT_REG_ADDR_BY_NAME(DAC_NODE, dac_reg))

/* DAC register offsets -- VERBATIM from dac_alif.c. */
#define OFF_DAC_REG1 0x00U /* control: EN(b0) / HP_MODE(b18) / TWOSCOMP(b22) / RESET_B(b27) */
#define OFF_DAC_IN   0x04U /* 12-bit input code */

/* DAC_REG1 control bits the driver sets during bring-up (dac_alif.c). */
#define DAC_EN_BIT       (1U << 0)  /* dac_enable(): channel enabled */
#define DAC_HP_MODE_BIT  (1U << 18) /* high-performance mode (driver leaves it as-is) */
#define DAC_TWOSCOMP_BIT (1U << 22) /* two's-complement input (off by default) */
#define DAC_RESET_B_BIT  (1U << 27) /* dac_reset_deassert(): reset de-asserted */

/* 12-bit DAC.  channel_setup rejects any resolution != 12 (-ENOTSUP). */
#define DAC_RESOLUTION 12U
#define DAC_MAX_CODE   ((1U << DAC_RESOLUTION) - 1U) /* 0xFFF */

/*
 * Full-scale DAC reference (mV) from the DT `alif,reference-mv` prop.  The
 * dac_alif driver fixes the DAC12 reference field to DAC12_VREF_CONT=0x4 =
 * 0.750 V (analog_ctrl.h:41 + table) -- so the dtsi sets alif,reference-mv=750
 * and this is the divisor for the REPORTED expected output.  Default 750 if the
 * prop is ever dropped.  This is the REGISTER reference; the absolute on-pad mV
 * is a bench unknown (unscoped pad), so the line below is REPORTED, not gated.
 */
#define DAC_REFERENCE_MV ((uint32_t)DT_PROP_OR(DAC_NODE, alif_reference_mv, 750))

/* Mid-scale code: half of full-scale (0x800).  Driven through DAC_IN. */
#define TEST_CODE (DAC_MAX_CODE / 2U + 1U) /* 0x800 */

/* The Alif DAC has a single output and no real channels (dac_write_data()
 * ARG_UNUSEDs the channel arg), but the Zephyr API still takes one. */
#define TEST_CHANNEL 0U

static inline uint32_t rd(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

int main(void)
{
	const struct device *dac = DEVICE_DT_GET(DAC_NODE);

	printk("\n=== aen-dac-regcheck ===\n");
	printk("dac node   : %s\n", DT_NODE_FULL_NAME(DAC_NODE));
	printk("dac_base   : 0x%08x\n", DAC_BASE);

	if (!device_is_ready(dac)) {
		printk("RESULT FAIL: dac device not ready (init/clock/VREF failed)\n");
		return 0;
	}

	/* Bring up the (single) channel at 12-bit.  dac_init() has already
	 * de-asserted RESET_B + programmed the trim; channel_setup sets the EN bit. */
	struct dac_channel_cfg ch_cfg = {
		.channel_id = TEST_CHANNEL,
		.resolution = DAC_RESOLUTION,
		.buffered   = false,
	};

	int rc_setup = dac_channel_setup(dac, &ch_cfg);
	printk("dac_channel_setup(ch=%u, res=%u) rc = %d\n", TEST_CHANNEL, DAC_RESOLUTION, rc_setup);

	/* Write a mid-scale code.  Bypass/input-mux is off by default, so the code
	 * lands directly in DAC_IN (dac_write_data). */
	int rc_write = dac_write_value(dac, TEST_CHANNEL, TEST_CODE);
	printk("dac_write_value(ch=%u, code=0x%03x) rc = %d\n", TEST_CHANNEL, TEST_CODE, rc_write);

	/*
	 * Register readback.  The DAC has no completion teardown / IRQ, so the
	 * programmed state persists: DAC_REG1 carries EN + RESET_B (and the trim
	 * bits), and DAC_IN retains the written 12-bit code.
	 */
	uint32_t reg1   = rd(DAC_BASE, OFF_DAC_REG1);
	uint32_t dac_in = rd(DAC_BASE, OFF_DAC_IN);

	printk("-- readback --\n");
	printk("DAC_REG1  0x%08x = 0x%08x (EN b0=%u RESET_B b27=%u HP b18=%u TWOSCOMP b22=%u)\n",
	       DAC_BASE + OFF_DAC_REG1,
	       reg1,
	       (reg1 & DAC_EN_BIT) ? 1U : 0U,
	       (reg1 & DAC_RESET_B_BIT) ? 1U : 0U,
	       (reg1 & DAC_HP_MODE_BIT) ? 1U : 0U,
	       (reg1 & DAC_TWOSCOMP_BIT) ? 1U : 0U);
	printk("DAC_IN    0x%08x = 0x%08x (12-bit code, exp 0x%03x)\n",
	       DAC_BASE + OFF_DAC_IN,
	       dac_in,
	       TEST_CODE);

	/*
	 * REPORTED (not gated): the expected ideal output for the written code at
	 * the driver-fixed 0.750 V reference (DAC12_VREF_CONT=0x4, analog_ctrl.h:41).
	 *   out_mv = code * reference_mv / full_scale_code
	 * = TEST_CODE * 750 / 0xFFF.  The pad is unscoped on this batch, so this is a
	 * REGISTER-derived prediction, not a measurement -- absolute on-pad mV is a
	 * bench/TRM unknown.  The aen-analog-validate example does the loopback check.
	 */
	uint32_t exp_out_mv = ((uint32_t)(TEST_CODE & DAC_MAX_CODE) * DAC_REFERENCE_MV) / DAC_MAX_CODE;

	printk("REPORTED  expected out = %u mV (code 0x%03x * %u mV VREF / 0x%03x; "
	       "ideal, pad unscoped)\n",
	       exp_out_mv,
	       (unsigned)(TEST_CODE & DAC_MAX_CODE),
	       DAC_REFERENCE_MV,
	       DAC_MAX_CODE);

	/*
	 * PASS gate:
	 *   - dac_channel_setup returned 0 (resolution accepted, EN set),
	 *   - dac_write_value returned 0 (code written),
	 *   - DAC_IN holds the written code (mask to 12 bits),
	 *   - DAC_REG1 has EN + RESET_B set (channel enabled, reset de-asserted).
	 * No measured-voltage check (the output pad is unscoped on this batch).
	 */
	bool code_ok = ((dac_in & DAC_MAX_CODE) == (TEST_CODE & DAC_MAX_CODE));
	bool ctrl_ok = ((reg1 & DAC_EN_BIT) != 0U) && ((reg1 & DAC_RESET_B_BIT) != 0U);

	bool ok = true;

	ok &= (rc_setup == 0);
	ok &= (rc_write == 0);
	ok &= code_ok;
	ok &= ctrl_ok;

	if (ok) {
		printk("RESULT PASS: dac0 setup+write rc=0, DAC_IN=0x%03x holds code, "
		       "DAC_REG1 EN+RESET_B set\n",
		       (unsigned)(dac_in & DAC_MAX_CODE));
	} else {
		printk("RESULT FAIL: setup_rc=%d write_rc=%d code_ok=%s ctrl_ok=%s "
		       "(dac_in=0x%08x reg1=0x%08x)\n",
		       rc_setup,
		       rc_write,
		       code_ok ? "OK" : "MISMATCH",
		       ctrl_ok ? "OK" : "NOT-SET",
		       dac_in,
		       reg1);
	}

	return 0;
}
