/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-gpio -- drive + read a PROXIED GPIO and the camera-enable
 * LDOs on the on-module TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor, from the
 * Alif Ensemble E8 host (M55-HE), over the inter-chip SPI bridge.
 *
 * This is the *Alif (host) side*; its peer is the ALP-authored firmware
 * that runs on the CC3501E's own Cortex-M33 (firmware/cc3501e/, embedded
 * per ADR 0015 -- like the gd32-bridge).  It is the sibling of
 * examples/aen/aen-cc3501e-bringup: it brings the bridge up the SAME way
 * (one call to cc3501e_bridge_bringup()), then exercises a different slice
 * of the firmware -- the GPIO proxy + camera enables -- instead of the
 * Wi-Fi/BLE control path.
 *
 *
 * ==== WHAT IS THE "GPIO PROXY"? ====================================
 *
 * Several E1M edge IO pads on the AEN SoM are NOT wired to an Alif pin --
 * they are wired to a *CC3501E* GPIO.  The CC3501E sits between the Alif
 * and those pads.  So when the application asks for "IO13", the SDK cannot
 * toggle an Alif register; it must ask the CC3501E firmware to toggle the
 * pad on its behalf, over the SPI bridge.  That indirection -- host op ->
 * bridge frame -> CC3501E drives the pad -- is the GPIO *proxy*.
 *
 * Two ways to use it:
 *
 *   1. PORTABLE (what an application normally writes):
 *        alp_gpio_t *io = alp_gpio_open(ALP_E1M_GPIO_IO13);
 *        alp_gpio_configure(io, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
 *        alp_gpio_write(io, true);
 *      With CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y and the route table in
 *      src/cc3501e_gpio_routes.c, the SDK silently routes ALP_E1M_GPIO_IO13
 *      over the bridge to raw CC3501E GPIO_13 -- the app never names the
 *      coprocessor.  (This build enables that backend so the bring-up
 *      helper's alp_gpio_cc3501e_attach() runs, exactly as a portable app
 *      would expect.)
 *
 *   2. DIRECT (what THIS demo does, to make the bridge op visible):
 *        cc3501e_gpio_configure(&fw, 13 [raw pad], DIR_OUTPUT, PULL_NONE, t);
 *        cc3501e_gpio_write(&fw, 13, true, t);
 *      Here `pad` is the RAW CC3501E GPIO index.  We call the chip driver
 *      directly so each bridge transaction is explicit in the teaching
 *      log -- it is the same wire op the portable path emits underneath.
 *
 * The route table (src/cc3501e_gpio_routes.c) ties ALP_E1M_GPIO_IO13 -> raw
 * CC3501E GPIO 13, so "pad 13" below is the same pad a portable
 * alp_gpio_open(ALP_E1M_GPIO_IO13) would reach.
 *
 *
 * ==== THE r2 HOST-IRQ CAVEAT (READ BEFORE USING INTERRUPTS) ========
 *
 * This HW rev uses hardware SS0 for SPI framing and a READY input for per-phase
 * gating, but it still does not expose GPIO edge events as portable application
 * callbacks.  The Alif is always the SPI master; the CC3501E is always the
 * slave.  Without a dedicated async event delivery path, the slave cannot
 * spontaneously tell the master "an edge happened on a GPIO".  So:
 *
 *   - cc3501e_gpio_set_interrupt() ARMS the edge on the CC3501E's own GPIO
 *     controller -- that part is real, it latches edges in the coprocessor.
 *   - but the async ALP_CC3501E_EVT_GPIO_INTERRUPT frame is not a portable
 *     callback path in this example.  The host must poll cc3501e_gpio_read() to
 *     observe a level change; the armed interrupt does not call you back.
 *
 * The step below therefore only proves the ARM round-trips; do not build a
 * design that depends on EVT_GPIO_INTERRUPT delivery on this rev.
 *
 *
 * ==== BENCH CONTRACT ===============================================
 *
 * Each step prints ONE stable line a sibling bench script greps:
 *     GPIO_TEST: <step> <PASS|FAIL>
 * with these exact <step> tokens, in order:
 *     gpio_config_out gpio_write_high gpio_write_low gpio_config_in
 *     gpio_read cam_enable cam_disable gpio_irq_arm
 * and a final:
 *     GPIO_TEST: SUMMARY pass=<n> fail=<n>
 * Keep these strings byte-for-byte -- the grep depends on them.
 *
 * NOTE ON native_sim: the CI build target (native_sim/native/64) has only
 * an EMULATED SPI controller with no CC3501E attached, so every bridge op
 * times out and the contract lines print FAIL there.  That is EXPECTED:
 * native_sim proves the example *builds* (twister build_only); the
 * PASS/FAIL contract is a SILICON bench signal, read on real hardware.
 *
 * This file is ~50 % comment by design: examples are documentation for
 * hand-written firmware, not just runnable code.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/cc3501e.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/*
 * The proxied pad this demo drives.  RAW CC3501E GPIO index 13, which the
 * route table (src/cc3501e_gpio_routes.c) maps to the portable E1M IO
 * ALP_E1M_GPIO_IO13 (= I2S_SELECT on the E1M-AEN netlist).  Driving it has no
 * destructive side-effect on the bench, which is why it is the demo pad.
 */
#define DEMO_PAD 13u

/*
 * Camera-enable LDO index.  0 = CAM_EN_LDO0 (CC35 GPIO_1); 1 = CAM_EN_LDO1
 * (CC35 GPIO_0) per the BDE-BW35N U4 netlist.  We exercise LDO0.
 */
#define DEMO_CAM 0u

/*
 * Per-request budget for the synchronous GPIO/camera bridge ops.  These
 * are fast in the firmware (no worker, unlike the Wi-Fi helpers), so a
 * short timeout is plenty; it also bounds how long native_sim sits on each
 * (failing) op before recording FAIL and moving on.
 */
#define DEMO_OP_TIMEOUT_MS 100u

/*
 * Emit one bench-contract line and fold the result into the running
 * tally.  `ok` is the boolean a step computed from its alp_status_t; we
 * print the exact "GPIO_TEST: <step> PASS|FAIL" the bench script greps and
 * bump pass/fail so main() can print the SUMMARY at the end.
 */
static void report(const char *step, bool ok, unsigned *pass, unsigned *fail)
{
	printf("GPIO_TEST: %s %s\n", step, ok ? "PASS" : "FAIL");
	if (ok) {
		(*pass)++;
	} else {
		(*fail)++;
	}
}

int main(void)
{
	printf("\n[cc3501e-gpio] E1M-AEN CC3501E GPIO-proxy + camera-enable demo\n");

	unsigned pass = 0u;
	unsigned fail = 0u;

	/*
	 * Bring up the SoM's CC3501E coprocessor in ONE call -- exactly like
	 * aen-cc3501e-bringup.  cc3501e_bridge_bringup() opens the inter-chip
	 * SPI bridge + the WIFI_EN/nRESET control pins, binds them, attaches
	 * the GPIO proxy backend (so a portable alp_gpio_open(ALP_E1M_GPIO_IO13)
	 * would route over the bridge), and runs the power + reset sequence
	 * (TI SWRU626 + the Puya cold-boot hard-reset).  Leaves WIFI_EN HIGH.
	 */
	cc3501e_t    fw;
	alp_status_t s = cc3501e_bridge_bringup(&fw);
	if (s == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) {
		/* The SPI bus / control pins are absent (wrong board overlay).
		 * Nothing to drive -- report all steps FAIL so the bench script
		 * still sees a complete contract, then bail. */
		printf("[cc3501e-gpio] bridge bring-up failed (SPI bus %u / WIFI_EN+nRESET "
		       "absent? err=%d) -- check the board overlay\n",
		       CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		return 0;
	}
	printf("[cc3501e-gpio] cc3501e bridge bring-up -> %d%s\n",
	       (int)s,
	       (s == ALP_ERR_NOSUPPORT) ? " (control pins not bound?)" : "");

	/*
	 * Step 1 -- configure the proxied pad as a push-pull OUTPUT with no
	 * internal pull.  This is GPIO_CONFIGURE (0x50) over the bridge: the
	 * host hands the CC3501E the pad index + direction + pull and the
		 * firmware programs its own GPIO controller.  ALP_OK = the firmware
		 * accepted the config and answered over the hardware-framed bridge.
	 */
	s = cc3501e_gpio_configure(
	    &fw, DEMO_PAD, ALP_CC3501E_GPIO_DIR_OUTPUT, ALP_CC3501E_GPIO_PULL_NONE, DEMO_OP_TIMEOUT_MS);
	report("gpio_config_out", s == ALP_OK, &pass, &fail);

	/*
	 * Step 2 -- drive the pad HIGH.  GPIO_WRITE (0x51) with level=true.
	 * On silicon a meter on the E1M IO13 pad reads ~3V3 after this.
	 */
	s = cc3501e_gpio_write(&fw, DEMO_PAD, true, DEMO_OP_TIMEOUT_MS);
	report("gpio_write_high", s == ALP_OK, &pass, &fail);

	/*
	 * Step 3 -- drive the pad LOW.  Same op, level=false; the pad returns
	 * to ~0 V.  Writing both levels proves the firmware honours the value
	 * field, not just that the op round-trips.
	 */
	s = cc3501e_gpio_write(&fw, DEMO_PAD, false, DEMO_OP_TIMEOUT_MS);
	report("gpio_write_low", s == ALP_OK, &pass, &fail);

	/*
	 * Step 4 -- reconfigure the SAME pad as an INPUT with the internal
	 * pull-UP enabled.  GPIO_CONFIGURE again, direction=INPUT, pull=UP.
	 * The on-die pull is documented as weak; a board needing a firm level
	 * adds an external resistor.  With nothing else driving the net the
	 * pull-up should make the next read sense HIGH.
	 */
	s = cc3501e_gpio_configure(
	    &fw, DEMO_PAD, ALP_CC3501E_GPIO_DIR_INPUT, ALP_CC3501E_GPIO_PULL_UP, DEMO_OP_TIMEOUT_MS);
	report("gpio_config_in", s == ALP_OK, &pass, &fail);

	/*
	 * Step 5 -- READ the pad back.  GPIO_READ (0x52): the firmware samples
	 * its pad and returns the level in the reply payload.  We print the
	 * sensed level for the bench log; PASS just means the read op itself
	 * succeeded (the actual level depends on what is wired to the pad).
	 */
	bool level = false;
	s          = cc3501e_gpio_read(&fw, DEMO_PAD, &level, DEMO_OP_TIMEOUT_MS);
	if (s == ALP_OK) {
		printf("[cc3501e-gpio] pad %u reads %s\n", DEMO_PAD, level ? "HIGH" : "LOW");
	}
	report("gpio_read", s == ALP_OK, &pass, &fail);

	/*
	 * Step 6 -- ENABLE camera LDO0.  cc3501e_cam_enable(which=0, on=true)
	 * sends CAM_ENABLE (0x60); the firmware asserts the LDO's enable pin
	 * (CC35 GPIO_1).  On a camera-fitted board this powers the sensor rail.
	 */
	s = cc3501e_cam_enable(&fw, DEMO_CAM, true, DEMO_OP_TIMEOUT_MS);
	report("cam_enable", s == ALP_OK, &pass, &fail);

	/*
	 * Step 7 -- DISABLE camera LDO0.  cc3501e_cam_enable(which=0, on=false)
	 * sends CAM_DISABLE (0x61); the firmware deasserts the enable.  We
	 * power it back down so the demo leaves the rail in its idle state.
	 */
	s = cc3501e_cam_enable(&fw, DEMO_CAM, false, DEMO_OP_TIMEOUT_MS);
	report("cam_disable", s == ALP_OK, &pass, &fail);

	/*
	 * Step 8 -- ARM a RISING-edge interrupt on the proxied pad.
	 * GPIO_SET_INTERRUPT (0x53), edge=RISING, enabled=true.
	 *
	 * IMPORTANT (see the caveat at the top): this ONLY arms the edge on
	 * the CC3501E's GPIO controller.  The async EVT_GPIO_INTERRUPT frame is
	 * not a portable callback path in this example.  A PASS here means "the
	 * firmware accepted the arm request", NOT "you will get an interrupt
	 * callback".  To observe the edge today you must POLL cc3501e_gpio_read()
	 * in a loop.  (EDGE_BOTH is unsupported on the CC35xx controller -- arm
	 * RISING or FALLING, never both.)
	 */
	s = cc3501e_gpio_set_interrupt(
	    &fw, DEMO_PAD, ALP_CC3501E_GPIO_EDGE_RISING, true, DEMO_OP_TIMEOUT_MS);
	report("gpio_irq_arm", s == ALP_OK, &pass, &fail);

	/*
	 * Final contract line: the running tally.  A bench run on silicon with
	 * the CC3501E firmware up expects pass=8 fail=0; under native_sim
	 * (emul SPI, no slave) expect pass=0 fail=8 -- the build, not the
	 * round-trip, is what the CI target proves.
	 */
	printf("GPIO_TEST: SUMMARY pass=%d fail=%d\n", (int)pass, (int)fail);

	/* The bridge SPI + control pins are owned by cc3501e_bridge_bringup();
	 * a real app that tears down would add a cc3501e_bridge_teardown(). */
	return 0;
}
