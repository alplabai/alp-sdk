/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-i3c-regcheck -- on-silicon controller-init validation of the SDK's
 * portable <alp/i3c.h> surface on the E1M-AEN801 (Ensemble E8, M55-HE),
 * via the bench RAM-run flow.
 *
 * BENCH-PROVEN 2026-07-25 (Flow C ITCM
 * RAM-run, reproduced twice, byte-identical).  Captured output:
 *
 *     bus: ALP_E1M_I3C0 = 0 (alp-i3c0 alias -> lpi3c0@0x43006000)
 *     alp_i3c_open: OK (handle=0x20000d20)
 *     capabilities: present (flags=0x00000000)
 *     alp_i3c_write(addr=0x08): status=-5 (ALP_ERR_IO -- expected, no
 *                                          target populated)
 *     RESULT PASS: I3C controller BINDS + OPENS via <alp/i3c.h> ...
 *
 * OBSERVING THIS APP ON THE BENCH: it selects the UART console below,
 * which is the customer-facing default -- but a Flow C ITCM RAM-run
 * produces ZERO bytes on UART5 (confirmed on hardware where UART5 capture
 * is otherwise proven working; the core reached arch_cpu_idle with
 * IPSR=0, i.e. it ran fine and the output simply did not route).  For a
 * RAM-run, layer the RAM console on WITHOUT editing this app:
 *
 *     west build ... -- -DEXTRA_CONF_FILE="scripts/bench/aen/aen-bench-shared.conf;scripts/bench/aen/aen-flowc-itcm.conf"
 *
 * then read ram_console_buf over SWD (scripts/bench/aen/ram-run.sh).  A
 * Flow A/D MRAM boot does reach UART5 normally.
 *
 * Those two committed fragments ARE the mechanism -- there is no
 * hand-written file to invent per invocation (#935).  aen-bench-shared.conf
 * carries the RAM console (and CONFIG_DCACHE=n); aen-flowc-itcm.conf carries
 * the ITCM link retarget a Flow C run also needs, and ram-run.sh refuses a
 * slot0-linked image with exit 5 rather than mis-running it.
 *
 * WHAT THIS APP VALIDATES (and what it deliberately does NOT):
 *
 *   The Alif I3C block is a Synopsys DesignWare I3C controller, for which
 *   UPSTREAM Zephyr already ships a full driver (drivers/i3c/i3c_dw.c,
 *   "snps,designware-i3c") -- pure ADR 0017 Tier-1 (upstream-native): no
 *   vendored or forked driver code, only the DT node (SoC overlay
 *   zephyr/dts/alif/ensemble_e8_peripherals.dtsi) + the board overlay.
 *
 *   This app drives the LP I3C instance (lpi3c0@0x43006000, IRQ 50) rather
 *   than the main i3c0.  The two are INDEPENDENT controllers that overlap
 *   on one pad pair only (P7_6/P7_7); the E1M-AEN801 breaks out just that
 *   pair, so on THIS SoM only one of them can be enabled at a time and
 *   firmware picks the owner.  On a board that routes them to separate
 *   pads both run at once.  We pick LP because it is the M55-HE local
 *   peripheral domain -- the core this app runs on.  The SoM pinout table
 *   does not make that choice for us (see the board overlay).
 *
 *   This app opens ALP_E1M_I3C0 through the portable <alp/i3c.h> dispatcher
 *   (alp_i3c_open -> the zephyr_drv backend -> the alp-i3c0 DT alias ->
 *   lpi3c0), which proves:
 *     1. the lpi3c0 DT node BINDS to "snps,designware-i3c" and
 *        device_is_ready() reports its init result (clock/pinctrl all ran
 *        during Zephyr device init),
 *     2. the alp-i3c0 alias resolves through the dispatcher's DT-alias
 *        table (COND_CODE_1 on DT_ALIAS(alp_i3c0)),
 *     3. alp_i3c_capabilities() returns a valid (if empty) descriptor for
 *        the opened handle,
 *     4. alp_i3c_write() reaches the driver's transfer path (address
 *        resolution + i3c_transfer()) and returns a well-formed
 *        alp_status_t -- NOT a hang, NOT a crash.
 *
 * WHAT IS UNTESTED ON THIS BATCH: a real transfer landing on a real target.
 * The E1M-AEN801 bench carrier has NO I3C target populated this batch
 * (reduced population -- see project memory), so dynamic address
 * assignment (DAA), which Zephyr's i3c_dw.c runs during device init for
 * any targets DECLARED in devicetree, finds ZERO targets (there is nothing
 * on the bus to declare).  A probe write to an arbitrary address therefore
 * has no device to resolve to -- the backend's i3c_dev_list_i3c_addr_find()
 * returns NULL and alp_i3c_write() reports ALP_ERR_IO, the SAME clean
 * "nothing answered" contract an I2C NACK would report.
 *
 * That ALP_ERR_IO is therefore an EXPECTED, PASSING result on this batch:
 * it is controller-init proof (the stack runs end-to-end and reports a
 * well-formed error), not transfer proof (no target exists to transfer
 * with).  Silicon verification of a real transfer is deferred to a
 * target-populated board -- the promotion gate documented in
 * include/alp/i3c.h's ABI-EXPERIMENTAL marker.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <alp/e1m_pinout.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>

/* No I3C target is populated on this bench carrier -- any address is
 * "nothing answers here".  0x08 is the first legal 7-bit-shaped address
 * (mirrors the I2C reserved-range floor), chosen only so the probe log
 * reads like a real register-read attempt. */
#define PROBE_ADDR 0x08u

int main(void)
{
	printk("\n=== aen-i3c-regcheck (<alp/i3c.h>, controller-init proof) ===\n");
	printk("bus: ALP_E1M_I3C0 = %u (alp-i3c0 alias -> lpi3c0@0x43006000)\n", ALP_E1M_I3C0);

	/*
	 * alp_i3c_open() resolves ALP_E1M_I3C0 through the zephyr_drv backend's
	 * DT-alias table, checks device_is_ready(), and returns a handle.  DAA
	 * for any declared targets already ran during Zephyr's device init --
	 * there is nothing to declare on this bench carrier, so it finds zero.
	 */
	alp_i3c_t *bus = alp_i3c_open(&ALP_I3C_CONFIG_DEFAULT(ALP_E1M_I3C0));

	if (bus == NULL) {
		printk("RESULT FAIL: alp_i3c_open failed (alp_last_error=%d; "
		       "expected NOT_READY=-2 if lpi3c0 not okay'd / clock / pinctrl)\n",
		       (int)alp_last_error());
		return 0;
	}
	printk("alp_i3c_open: OK (handle=%p)\n", (void *)bus);

	/* Capabilities: an empty-but-valid descriptor proves the open path
	 * populated caps_out, even though this backend advertises no flags. */
	const alp_capabilities_t *caps = alp_i3c_capabilities(bus);

	printk("capabilities: %s (flags=0x%08x)\n",
	       (caps != NULL) ? "present" : "NULL",
	       (caps != NULL) ? caps->flags : 0u);

	/*
	 * Probe write: reaches the driver's transfer path (address resolution +
	 * i3c_transfer()).  EXPECTED result on this batch is ALP_ERR_IO -- no
	 * target answers addr 0x08 because none is populated -- see the file
	 * header.  Any OTHER outcome (a hang, a crash, or an unrecognised
	 * status) would indicate a real bug in the backend.
	 */
	uint8_t      probe_byte = 0xAAu;
	alp_status_t wr         = alp_i3c_write(bus, PROBE_ADDR, &probe_byte, 1u);
	const char  *wr_note    = (wr == ALP_ERR_IO) ? "ALP_ERR_IO -- expected, no target populated"
	                          : (wr == ALP_OK)   ? "ALP_OK -- unexpected on this batch"
	                          : (wr == ALP_ERR_NOSUPPORT) ? "ALP_ERR_NOSUPPORT -- CONFIG_I3C off?"
	                                                      : "unexpected status";

	printk("alp_i3c_write(addr=0x%02x): status=%d (%s)\n", PROBE_ADDR, (int)wr, wr_note);

	/*
	 * PASS gate: the controller BINDS + OPENS + reports well-formed
	 * capabilities, and the probe write reaches the transfer path and
	 * returns a clean status (ALP_ERR_IO is the expected "no target"
	 * outcome on this batch; ALP_OK would also be acceptable evidence the
	 * call path works, in case a target ever IS populated).  A crash, hang,
	 * or ALP_ERR_NOSUPPORT (subsystem not built) is the failure signature
	 * this gate catches.
	 */
	/* No `caps != NULL` term here: alp_i3c_capabilities() returns non-NULL
	 * for any non-NULL handle, and `bus` was already NULL-checked above, so
	 * such a term can never be false -- it would dress up the PASS line with
	 * a check that cannot fail.  The falsifiable evidence is the probe
	 * status alone. */
	bool probe_ok = (wr == ALP_ERR_IO) || (wr == ALP_OK);

	if (probe_ok) {
		printk("RESULT PASS: I3C controller BINDS + OPENS via <alp/i3c.h> -- "
		       "lpi3c0 ready, alp-i3c0 alias resolves, probe write returns a "
		       "well-formed status (%d). Controller-init proof only: no I3C "
		       "target is populated this batch, so live transfer is UNTESTED.\n",
		       (int)wr);
	} else {
		printk("RESULT FAIL: I3C controller not fully staged "
		       "(caps=%p write_status=%d)\n",
		       (void *)caps,
		       (int)wr);
	}

	alp_i3c_close(bus);
	return 0;
}
