/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon analog-comparator (HSCMP) validation for the E1M-AEN801 (Alif
 * Ensemble E8, M55-HE), over the alp-sdk Tier-2 comparator_alif driver (ADR 0017
 * Tier-2, "alif,cmp" -- a clean-room driver implementing the PORTABLE Zephyr
 * comparator class API for cmp0@49023000).
 *
 * What it proves -- INTERNAL-reference smoke (NO external wiring)
 * -------------------------------------------------------------
 * The comparator's negative input is set, in the DT node, to the on-chip DAC6
 * programmable reference (negative_input = "CMP_NEG_IN3" -> COMP_HS_IN_M_SEL =
 * 0x3 = DAC6).  The driver turns DAC6 on through the hal_alif analog_ctrl helper,
 * so the comparator has a known INTERNAL reference on its minus terminal with NO
 * bench wiring.  This app then exercises the comparator over the portable class
 * API and confirms the path is alive on silicon:
 *
 *   1. device readiness   -- the cmp0 node instantiated a comparator device.
 *   2. comparator_get_output() returns a clean 0/1 (NOT an -errno) -- i.e. the
 *      driver read CMP_STATUS.CMP_VALUE through the IP without faulting.
 *   3. comparator_set_trigger(BOTH_EDGES) + a callback are accepted (the IRQ is
 *      wired and the mask register is writable).
 *   4. comparator_set_trigger(NONE) re-masks cleanly.
 *
 * The ABSOLUTE output LEVEL is NOT asserted: the positive input is the CMP0_IN0
 * analog pad, which is UNROUTED on this bench (no external source), so its
 * voltage vs the ~0.9 V DAC6 reference is indeterminate -- whatever bit comes
 * back is REPORTED, not failed.  The PASS gate is "the driver drove the HSCMP IP
 * through the portable comparator_* API and every call returned a valid,
 * non-error result", exactly the bar the ADC regcheck uses for an unrouted pad.
 *
 *   --- over J-Link (ground truth, independent of any printk) ---
 *   mem32 0x49023018 = cmp0 CMP_STATUS; bit 0 (CMP_VALUE) is the live comparator
 *   output.  mem32 0x49023000 = CMP_COMP_REG1; bit 28 (COMP_HS_EN) must read 1
 *   after init (the comparator is enabled).
 *
 * BENCH CONFIG -- the EXTERNAL input pin + threshold (driving CMP0_IN0 from a
 * known voltage and asserting the crossing) is bench wiring that is NOT set here;
 * it is a TBD (see the README).  This smoke deliberately stays self-contained on
 * the internal DAC6 reference.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/comparator.h>

/* The comparator device is the "alif,cmp" node cmp0@49023000; the
 * comparator_alif driver binds it as a Zephyr comparator-class device. */
#define CMP_NODE DT_NODELABEL(cmp0)

static const struct device *const cmp = DEVICE_DT_GET(CMP_NODE);

/* Set by the trigger callback so we can confirm the callback path is wired.
 * volatile: written from ISR context, read from the main loop. */
static volatile uint32_t cb_count;

static void cmp_trigger_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	cb_count++;
}

int main(void)
{
	int out0, out1, rc;

	printk("\n=== AEN801 CMP (HSCMP) internal-reference bench "
	       "(comparator_alif / cmp0@49023000) ===\n");

	/* 1. device readiness.  If the cmp0 node did not instantiate a device the
	 *    build would have failed at link (undefined __device_dts_ord_*), so
	 *    reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(cmp)) {
		printk("RESULT FAIL: cmp device not ready\n");
		return 0;
	}
	printk("cmp device ready (neg input = DAC6 internal reference)\n");

	/* 2. read the comparator output through the portable class API.  A valid
	 *    return is exactly 0 or 1; anything negative is an -errno from the
	 *    driver and is a failure.  (The LEVEL itself is not asserted -- the
	 *    plus pad is unrouted on this bench.) */
	out0 = comparator_get_output(cmp);
	printk("comparator_get_output() #1 = %d\n", out0);
	if (out0 < 0) {
		printk("RESULT FAIL: get_output returned -errno %d\n", out0);
		return 0;
	}

	/* --- J-Link readback: mem32 0x49023018 (cmp0 CMP_STATUS, bit0=CMP_VALUE),
	 *     mem32 0x49023000 (CMP_COMP_REG1, bit28=COMP_HS_EN must be 1) --- */

	/* 3. arm an edge trigger + callback: proves the IRQ is wired and the mask
	 *    register is writable.  BOTH_EDGES so any movement on the (unrouted)
	 *    input that does cross the reference would be caught. */
	rc = comparator_set_trigger_callback(cmp, cmp_trigger_cb, NULL);
	printk("comparator_set_trigger_callback() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: set_trigger_callback rc=%d\n", rc);
		return 0;
	}

	rc = comparator_set_trigger(cmp, COMPARATOR_TRIGGER_BOTH_EDGES);
	printk("comparator_set_trigger(BOTH_EDGES) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: set_trigger rc=%d\n", rc);
		return 0;
	}

	/* Give the comparator a moment; with an unrouted plus pad we do NOT expect
	 * a guaranteed edge, so cb_count is reported, not asserted. */
	k_msleep(50);

	/* 4. read again, then re-mask cleanly. */
	out1 = comparator_get_output(cmp);
	printk("comparator_get_output() #2 = %d  (callbacks seen=%u)\n", out1, cb_count);
	if (out1 < 0) {
		printk("RESULT FAIL: get_output #2 returned -errno %d\n", out1);
		return 0;
	}

	rc = comparator_set_trigger(cmp, COMPARATOR_TRIGGER_NONE);
	printk("comparator_set_trigger(NONE) rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: set_trigger(NONE) rc=%d\n", rc);
		return 0;
	}

	/* 5. verdict: the HSCMP IP was driven through the full portable
	 *    comparator_* API and every call returned a valid, non-error result.
	 *    The output bit (out0/out1) is informational -- the plus pad is
	 *    unrouted, so the level vs the DAC6 reference is indeterminate. */
	printk("RESULT PASS: comparator_alif drove HSCMP cmp0 via the portable "
	       "comparator_* API (out=%d/%d, internal DAC6 reference, "
	       "external pin/threshold = bench TBD)\n",
	       out0,
	       out1);

	return 0;
}
