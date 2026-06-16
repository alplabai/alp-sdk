/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-xspi-flash-readwrite -- erase one page on the V2N's on-module
 * xSPI NOR, write a known pattern, read it back, compare.
 *
 * V2N populates an xSPI NOR flash on the board-facing xSPI bus
 * (see metadata/e1m_modules/v2n/renesas-peripheral-map.tsv for the
 * pad assignment).  Zephyr's flash subsystem abstracts the chip
 * behind a `flash` driver class; this example uses the standard
 * `flash_read` / `flash_erase` / `flash_write` API so it works on
 * any board file that exposes the chip via the `zephyr,flash` DT
 * binding.
 *
 * The example writes at the *start of the highest 4-KiB sector* on
 * a 256-Mbit (32 MiB) part: offset 0x1FFF000.  Boards that ship a
 * different part size or partition layout can override
 * `XSPI_TEST_OFFSET` at build time.
 *
 * **Safety:** this example erases + writes flash.  Don't run it on
 * a board whose xSPI NOR holds anything you care about -- by
 * default it touches the last 4 KiB only, but that's still
 * destructive.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>

/* Offset to erase / program / verify.  4 KiB is the smallest
 * eraseable unit on virtually every consumer-grade NOR; one sector
 * is comfortably below the 256-byte page-program ceiling so the
 * write loop below doesn't need to chunk. */
#ifndef XSPI_TEST_OFFSET
#define XSPI_TEST_OFFSET 0x01FFF000u
#endif

#define XSPI_TEST_BYTES 256u /* one programming page on 25-series parts */

/* The "known pattern" we write.  A repeating ramp 0x00..0xFF is
 * easier on the eye than a single fill byte when scoping the
 * read-back -- any single missed byte stands out. */
static uint8_t pattern[XSPI_TEST_BYTES];

static void build_pattern(void)
{
	for (size_t i = 0u; i < sizeof pattern; ++i) {
		pattern[i] = (uint8_t)i;
	}
}

/* Resolve the xSPI NOR via the `xspi-flash` DT alias.  The V2N
 * board file (alplabai/alp-zephyr-modules) is responsible for
 * setting `aliases { xspi-flash = &mx25l25645g; };` or whatever
 * matches the populated part.  Boards without the alias (native_sim,
 * AEN, IMX9) compile to a NULL device pointer + the runtime check
 * below short-circuits so the same source builds across the SDK's
 * twister matrix.  DEVICE_DT_GET would otherwise fail at link time
 * with `__device_dts_ord_DT_N_ALIAS_xspi_flash_ORD undeclared`. */
#define XSPI_FLASH_NODE DT_ALIAS(xspi_flash)
#if DT_NODE_HAS_STATUS(XSPI_FLASH_NODE, okay)
#define XSPI_FLASH_DEV DEVICE_DT_GET(XSPI_FLASH_NODE)
#else
#define XSPI_FLASH_DEV NULL
#endif

int main(void)
{
	printf("[xspi] v2n-xspi-flash-readwrite\n");

	const struct device *flash = XSPI_FLASH_DEV;
	if (flash == NULL || !device_is_ready(flash)) {
		printf("[xspi] xspi-flash alias not present or device not ready\n");
		return 0;
	}
	printf("[xspi] using device: %s\n", flash->name);

	build_pattern();

	/* Step 1: erase the target 4-KiB sector.  flash_erase rejects
     * offsets that aren't sector-aligned + sizes that aren't
     * multiples of the sector size; the offset we picked aligns. */
	int rv = flash_erase(flash, XSPI_TEST_OFFSET, 4096u);
	if (rv != 0) {
		printf("[xspi] flash_erase(0x%08x, 4096) -> %d\n", (unsigned)XSPI_TEST_OFFSET, rv);
		return 0;
	}

	/* Step 2: read the freshly-erased region back.  NOR returns
     * 0xFF for every byte of an erased sector; if anything else
     * shows up the erase failed silently and the write below is
     * going to mis-program. */
	uint8_t scratch[XSPI_TEST_BYTES];
	rv = flash_read(flash, XSPI_TEST_OFFSET, scratch, sizeof scratch);
	if (rv != 0) {
		printf("[xspi] flash_read after erase -> %d\n", rv);
		return 0;
	}
	bool clean = true;
	for (size_t i = 0u; i < sizeof scratch; ++i) {
		if (scratch[i] != 0xFFu) {
			clean = false;
			break;
		}
	}
	printf("[xspi] post-erase read: %s\n", clean ? "all 0xFF (clean)" : "DIRTY");
	if (!clean) return 0;

	/* Step 3: write the pattern.  Page-program is destructive --
     * NOR ANDs the input with the on-flash bytes -- but the sector
     * is freshly erased to 0xFF so we get the pattern verbatim. */
	rv = flash_write(flash, XSPI_TEST_OFFSET, pattern, sizeof pattern);
	if (rv != 0) {
		printf("[xspi] flash_write -> %d\n", rv);
		return 0;
	}

	/* Step 4: read it back + compare. */
	memset(scratch, 0u, sizeof scratch);
	rv = flash_read(flash, XSPI_TEST_OFFSET, scratch, sizeof scratch);
	if (rv != 0) {
		printf("[xspi] flash_read after write -> %d\n", rv);
		return 0;
	}
	const int diff = memcmp(pattern, scratch, sizeof scratch);
	printf("[xspi] readback %s (memcmp = %d)\n", diff == 0 ? "OK" : "DIFFERS", diff);

	/* Optional: dump the first 16 bytes so the log shows the
     * pattern explicitly.  Useful in CI artifacts where comparing
     * captured stdout is the only way to confirm correctness
     * without re-running the binary. */
	printf("[xspi] first 16 read: ");
	for (size_t i = 0u; i < 16u; ++i)
		printf("%02X", scratch[i]);
	printf("\n");

	printf("[xspi] done\n");
	return 0;
}
