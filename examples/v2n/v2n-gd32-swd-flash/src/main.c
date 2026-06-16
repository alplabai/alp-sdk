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
 * three GPIOs (SWDIO + SWCLK + optional NRST) routed from the
 * Renesas host to the GD32, no external probe required.  The path
 * stays available even when the application-bootloader OTA path is
 * unreachable (corrupt bridge image, factory first-flash).
 *
 * **Pin assignment (resolved 2026-05-12):** SWDIO -> Renesas `P70`,
 * SWCLK -> Renesas `P71`, NRST -> Renesas `P74` (open-drain, shared
 * with the primary PMIC reset-out).  Authoritative source:
 * `metadata/chips/gd32_swd.yaml` + `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`.
 * The studio's pin allocator resolves these to the integer pin ids
 * below at build time from the board preset.  If the board
 * preset hasn't propagated the routing yet, the alp_gpio_open call
 * below will return NULL -- the example prints the failure and exits
 * cleanly rather than wedging the host.
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

/* Studio-resolved pin ids for the V2N gd32_swd routing.  The
 * board preset (`metadata/boards/E1M-X-EVK/board.yaml`) declares
 * the gd32_swd block; `alp_project.py` emits these as part of the
 * studio's generated build.  The integer values below are the
 * canonical preset ordering (SWDIO first, SWCLK second, NRST third)
 * resolved against the board's auxiliary-GPIO array -- if the
 * board shuffles the array the example follows the board. */
#define V2N_GD32_SWDIO_PIN_ID 0u /* Renesas P70 on V2N */
#define V2N_GD32_SWCLK_PIN_ID 1u /* Renesas P71 on V2N */
#define V2N_GD32_NRST_PIN_ID 2u  /* Renesas P74 (open-drain) on V2N */

/* Write target: the top sector of the GD32G553's 512 KB flash.
 * 0x08080000 - 2048 = 0x0807F800 is the last sector start address. */
#define WRITE_ADDR 0x0807F800u
#define WRITE_BYTES 64u

static uint8_t pattern[WRITE_BYTES];
static uint8_t scratch[WRITE_BYTES];

static void    build_pattern(void)
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

	/* Open the three GPIO handles.  Each call resolves a
     * studio-supplied pin id; failure usually means the board
     * preset hasn't routed the SWD lines yet (still TBD on the V2N
     * schematic). */
	alp_gpio_t *swdio = alp_gpio_open(V2N_GD32_SWDIO_PIN_ID);
	alp_gpio_t *swclk = alp_gpio_open(V2N_GD32_SWCLK_PIN_ID);
	alp_gpio_t *nrst  = alp_gpio_open(V2N_GD32_NRST_PIN_ID);
	if (swdio == NULL || swclk == NULL) {
		printf("[swd] alp_gpio_open failed (SWDIO + SWCLK required); "
		       "board may not have routed SWD lines yet\n");
		alp_gpio_close(swdio);
		alp_gpio_close(swclk);
		alp_gpio_close(nrst);
		return 0;
	}
	/* NRST is optional -- boards that don't route it work via
     * software AIRCR.SYSRESETREQ in the driver's reset path. */
	if (nrst == NULL) {
		printf("[swd] note: NRST not opened; software-reset fallback will be used\n");
	}

	/* Bind the SWD controller.  init configures both lines as
     * outputs at the SWD idle state + leaves NRST released. */
	gd32_swd_t   swd;
	alp_status_t s = gd32_swd_init(&swd, swdio, swclk, nrst);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_init -> %d\n", (int)s);
		goto out;
	}

	/* Connect: line reset + JTAG-to-SWD switch + DPIDR read.
     * On a real GD32G553 we expect IDCODE = 0x6BA02477 (Cortex-M33
     * r0p1 SW-DPv2).  Anything else means a different silicon
     * answered or the wire is mis-routed. */
	s = gd32_swd_connect(&swd);
	if (s != ALP_OK) {
		printf("[swd] gd32_swd_connect -> %d "
		       "(target not responding or wire issue)\n",
		       (int)s);
		goto deinit;
	}
	printf("[swd] connected -- IDCODE = 0x%08X (expected 0x%08X)\n", (unsigned)swd.idcode,
	       (unsigned)GD32_SWD_EXPECTED_IDCODE);
	if (swd.idcode != GD32_SWD_EXPECTED_IDCODE) {
		printf("[swd] WARN: IDCODE mismatch -- this isn't a GD32G553?\n");
		/* Continue anyway -- the FMC layout might still match if
         * the silicon is a pin-compatible variant.  Real production
         * test should refuse to proceed on a mismatch.   */
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
		printf("[swd] gd32_swd_flash_erase(0x%08X, %u) -> %d\n", (unsigned)WRITE_ADDR,
		       (unsigned)WRITE_BYTES, (int)s);
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
