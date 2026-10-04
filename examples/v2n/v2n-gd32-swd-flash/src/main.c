/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gd32-swd-flash -- demonstrate the host-driven SWD bit-bang
 * controller (chips/gd32_swd/) by attaching to the on-module
 * GD32G553, reading the SW-DP IDCODE, halting the Cortex-M33, and
 * writing a tiny scratch pattern to the last flash sector then
 * reading it back.
 *
 * This is the recovery / first-flash path documented in
 * docs/gd32-bridge-protocol.md §10 Path B + docs/bring-up-v2n.md §2b:
 * three GPIOs (SWDIO + SWCLK + NRST) routed from the Renesas host to
 * the GD32, no external probe required.  The path stays available even
 * when the application-bootloader OTA path is unreachable (corrupt
 * bridge image, factory first-flash).
 *
 * **Pin assignment (resolved 2026-05-12):** SWDIO -> Renesas `P70`,
 * SWCLK -> Renesas `P71`, NRST -> Renesas `P74` (open-drain, shared
 * with the primary PMIC reset-out).  Authoritative source:
 * `metadata/chips/gd32_swd.yaml` + `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`.
 * The CM33 board publishes these as the `swdio` / `swclk` / `nrst` pads of
 * its dedicated `alp,gd32-pads` devicetree node (generated from
 * `metadata/e1m_modules/v2n/supervisor-links.yaml`), reached through the
 * reserved pad ids GD32G553_PAD_ID_*.  They are deliberately NOT indices of
 * the positional pin array: index 0 of that array is the GD32 SPI
 * chip-select (Renesas P97 = GD32 PA8), so a drifted index would drive it.
 * If the board does not publish the pads, alp_gpio_open returns NULL and
 * the example prints the failure and exits cleanly.
 *
 * **Connect-under-reset, and why NRST is mandatory.**  P71 is also the
 * bridge's ATTN input (GD32 PA14), which the GD32 drives while the v0.15
 * ATTN link feature is granted -- so the host may only drive it as an
 * SWD clock output while GD32_NRST holds the GD32 in reset.  gd32_swd_init()
 * asserts NRST first (and refuses a NULL one), gd32_swd_connect() arms
 * halt-on-reset and only then releases it, so the core stops at its reset
 * vector before any application code runs.  For the whole session the
 * supervisor closes the SPI bridge link and answers every bridge command
 * BUSY; gd32_swd_deinit() gives P70/P71 back as inputs.
 *
 * **Safety:**
 *   * This example writes to the *last sector* of the GD32G553's
 *     flash (top 2 KB) by default to minimise impact on any
 *     in-place bridge firmware.
 *   * If the bridge firmware happens to live in that sector the
 *     write trashes it; flash a fresh bridge image afterwards via
 *     either an external probe or the same SWD driver.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/chips/gd32_swd.h"
#include "alp/chips/gd32g553.h"

/* The three pads, as reserved ids resolved from the board's `alp,gd32-pads`
 * devicetree node (see the file header). */
#define V2N_GD32_SWDIO_PIN_ID GD32G553_PAD_ID_SWDIO /* Renesas P70 on V2N */
#define V2N_GD32_SWCLK_PIN_ID GD32G553_PAD_ID_SWCLK /* Renesas P71 on V2N */
#define V2N_GD32_NRST_PIN_ID  GD32G553_PAD_ID_NRST  /* Renesas P74 (open-drain) on V2N */

/* Write target: the top sector of the GD32G553's 512 KB flash.
 * 0x08080000 - 2048 = 0x0807F800 is the last sector start address. */
#define WRITE_ADDR  0x0807F800u
#define WRITE_BYTES 64u

static uint8_t pattern[WRITE_BYTES];
static uint8_t scratch[WRITE_BYTES];

static void build_pattern(void)
{
	/* Repeating 0x10..0x4F ramp -- chosen so a single missed byte
     * stands out in the readback log. */
	for (size_t i = 0u; i < sizeof pattern; ++i) {
		pattern[i] = (uint8_t)(0x10u + i);
	}
}

int main(void)
{
	printf("[swd] v2n-gd32-swd-flash\n");

	/* Open the three GPIO handles.  Each call resolves a reserved pad id
	 * from the board's alp,gd32-pads node; failure means the board does
	 * not publish the SWD pads (the pads themselves are resolved:
	 * P70/P71/P74, maintainer-confirmed 2026-05-12; see
	 * metadata/chips/gd32_swd.yaml). */
	alp_gpio_t *swdio = alp_gpio_open(V2N_GD32_SWDIO_PIN_ID);
	alp_gpio_t *swclk = alp_gpio_open(V2N_GD32_SWCLK_PIN_ID);
	alp_gpio_t *nrst  = alp_gpio_open(V2N_GD32_NRST_PIN_ID);
	if (swdio == NULL || swclk == NULL || nrst == NULL) {
		printf("[swd] alp_gpio_open failed (SWDIO + SWCLK + NRST all required); "
		       "board does not publish the GD32 pads\n");
		alp_gpio_close(swdio);
		alp_gpio_close(swclk);
		alp_gpio_close(nrst);
		return 0;
	}
	/* Bind the SWD controller.  init closes the bridge link (the supervisor
	 * hook), ASSERTS NRST, and only then configures both lines as outputs at
	 * the SWD idle state; NRST stays asserted until connect() releases it. */
	gd32_swd_t   swd;
	alp_status_t s = gd32_swd_init(&swd, swdio, swclk, nrst);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_init -> %d\n", (int)s);
		goto out;
	}

	/* Connect (under reset): line reset + JTAG-to-SWD switch + DPIDR read, then
	 * enable debug + halt-on-reset and release NRST so the core stops at its
	 * reset vector.
     * GD32_SWD_GENERIC_CM33_R0P1_IDCODE (0x6BA02477) is the GENERIC
     * Cortex-M33 r0p1 SW-DPv2 architectural default, never measured on
     * a GD32G553 with a probe attached (see the @warning on the macro
     * in include/alp/chips/gd32_swd.h). Whether a real GD32G553 matches
     * it or not is UNKNOWN (#1440, #1369) -- logged for information
     * only; neither a match nor a mismatch here is evidence of correct
     * or wrong silicon. */
	s = gd32_swd_connect(&swd);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_connect -> %d "
		       "(target not responding or wire issue)\n",
		       (int)s);
		goto deinit;
	}
	printf("[swd] connected -- IDCODE = 0x%08X (generic reference 0x%08X)\n",
	       (unsigned)swd.idcode,
	       (unsigned)GD32_SWD_GENERIC_CM33_R0P1_IDCODE);
	if (swd.idcode != GD32_SWD_GENERIC_CM33_R0P1_IDCODE) {
		printf("[swd] note: IDCODE != generic reference -- this is not a "
		       "wrong-board signal, the reference value is unattested "
		       "on a GD32 (#1440, #1369)\n");
		/* Continue anyway: GD32_SWD_GENERIC_CM33_R0P1_IDCODE is not a
         * GD32 measurement, so a mismatch proves nothing.  Real
         * production test should refuse to proceed on a mismatch
         * against a value it has measured on its own board. */
	}

	/* Halt the Cortex-M33 so any running application stops
     * touching FMC concurrently with our writes. */
	s = gd32_swd_halt(&swd);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_halt -> %d\n", (int)s);
		goto deinit;
	}
	printf("[swd] target halted\n");

	/* Erase the target sector.  Address + size are rounded out to
     * sector boundaries; passing 64 bytes erases the enclosing
     * 2 KiB sector. */
	s = gd32_swd_flash_erase(&swd, WRITE_ADDR, WRITE_BYTES);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_flash_erase(0x%08X, %u) -> %d\n",
		       (unsigned)WRITE_ADDR,
		       (unsigned)WRITE_BYTES,
		       (int)s);
		goto reset;
	}

	/* Build + write the pattern. */
	build_pattern();
	s = gd32_swd_flash_write(&swd, WRITE_ADDR, pattern, WRITE_BYTES);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_flash_write -> %d\n", (int)s);
		goto reset;
	}

	/* Verify by reading back + comparing.  The driver implements
     * verify via AHB-AP memory reads -- doesn't need the FMC
     * controller, so this works even on a half-bricked chip. */
	s = gd32_swd_flash_verify(&swd, WRITE_ADDR, pattern, WRITE_BYTES);
	printf("[swd] flash_verify -> %d (%s)\n", (int)s, s == ALP_OK ? "OK" : "mismatch");

	/* Dump the first 16 bytes via a fresh verify against a probe
     * buffer so the log shows the actual bytes on flash. */
	memset(scratch, 0u, sizeof scratch);
	/* We don't have a public "read N bytes" helper -- verify with
     * a known buffer is the closest the driver offers.  For
     * debugging we'd extend the driver with a raw memory-read
     * helper; that's a follow-up. */

reset:
	/* Release the core, reset, and run the existing firmware. */
	s = gd32_swd_reset_and_run(&swd);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_reset_and_run -> %d\n", (int)s);
	} else {
		printf("[swd] target reset + running\n");
	}

deinit:
	gd32_swd_deinit(&swd);

out:
	alp_gpio_close(swdio);
	alp_gpio_close(swclk);
	alp_gpio_close(nrst);
	printf("[swd] done\n");
	return 0;
}
