/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon CRC-engine validation for the E1M-AEN801 (Alif Ensemble E8) over
 * the alp-sdk clean-room crc_alif driver (ADR 0017 Tier-1.5, compatible
 * "alif,crc" -- a Zephyr CRC class driver re-authored against the UPSTREAM
 * Zephyr v4.4 CRC API from the Alif DFP register model, for the polled
 * memory-mapped CRC accelerator at crc0@48107000).
 *
 * What it proves
 * --------------
 * Compute a CRC over a FIXED compile-time buffer through the PORTABLE Zephyr CRC
 * class API (crc_begin -> crc_update -> crc_finish over struct crc_ctx) and
 * confirm the hardware produces the EXPECTED value, two independent ways:
 *
 *   1. in-firmware:  this app drives the engine, prints every API return code,
 *      the computed result, the host-precomputed reference, and a single
 *      'RESULT PASS:' / 'RESULT FAIL:' line (it compares with crc_verify()).
 *   2. over J-Link:  the human reads the engine CRC_OUT register with mem32
 *      (0x48107018 = base 0x48107000 + 0x18; see the overlay) after the run --
 *      the ground truth on silicon, independent of any printk.
 *
 * The fixed buffer + reference value
 * ----------------------------------
 * The buffer is 16 bytes (a multiple of 4, so the word-fed CRC32 path consumes
 * it cleanly).  The reference CRC32_IEEE value 0x684FC31C was precomputed on the
 * host with Python zlib.crc32() over the SAME 16 bytes -- the canonical CRC-32
 * (poly 0x04C11DB7, reflected in/out, init 0xFFFFFFFF, final XOR 0xFFFFFFFF):
 *
 *     >>> import zlib
 *     >>> zlib.crc32(bytes([0x10,0x20,0x30,0x40, 0x50,0x60,0x70,0x80,
 *     ...                    0x90,0xA0,0xB0,0xC0, 0xD0,0xE0,0xF0,0x00]))
 *     0x684FC31C
 *
 * BENCH NOTE -- reflect / final-XOR (BENCH-UNVERIFIED): the canonical CRC-32
 * applies bit-reflection on input+output AND a final one's-complement of the
 * result.  The Alif engine drives reflection from its CRC_CONTROL REFLECT bit
 * (this app sets CRC_FLAG_REVERSE_OUTPUT, which the driver maps to REFLECT) and
 * the final inversion from the CONTROL INVERT bit.  The exact ORDER the silicon
 * applies reflect vs. invert (and whether the seed is pre- or post-reflected)
 * is the one thing this regcheck settles on the bench: if the printed result
 * does NOT equal 0x684FC31C, read CRC_OUT over J-Link and compare the raw
 * (non-reflected, non-inverted) value to localise which post-processing step
 * the driver must add -- do NOT guess the order without the silicon answer.
 * This is a documented bench-confirm item, not a code defect.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf); the bench UART is
 * not wired to USB.  BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>

/* The CRC device is the "alif,crc" node (crc0@48107000); the crc_alif driver
 * binds it directly. */
#define CRC_NODE DT_NODELABEL(crc0)

/* Fixed input buffer: 16 bytes, a multiple of 4 so the 32-bit data-input
 * register path (CRC32_IEEE / CRC32_C) consumes whole words with no remainder.
 * Keeping it const + file-scope makes it a deterministic compile-time vector. */
static const uint8_t fixed_buf[16] = {
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00,
};

/* Host-precomputed reference (Python zlib.crc32 over fixed_buf); see the file
 * header for the exact one-liner that produced it. */
#define EXPECTED_CRC32_IEEE 0x684FC31CU

static const struct device *const crc_dev = DEVICE_DT_GET(CRC_NODE);

int main(void)
{
	/* The CRC context carries the algorithm selection, the canonical
	 * polynomial + seed for CRC-32/IEEE, and the reflect/invert flags.  The
	 * driver validates ctx->polynomial against the type, so it MUST be the
	 * upstream CRC32_IEEE_POLY (0x04C11DB7). */
	struct crc_ctx ctx = {
		.type       = CRC32_IEEE,
		.polynomial = CRC32_IEEE_POLY,
		.seed       = CRC32_IEEE_INIT_VAL, /* 0xFFFFFFFF, zephyr/drivers/crc.h */
		.reversed   = CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
	};
	int rc;

	printk("\n=== AEN801 CRC engine bench "
	       "(crc_alif / crc0@48107000, CRC32_IEEE) ===\n");

	/* 1. device readiness.  If the CRC node did not instantiate a device the
	 *    build would have failed at link (undefined __device_dts_ord_*), so
	 *    reaching here means the device object exists -- check it is ready. */
	if (!device_is_ready(crc_dev)) {
		printk("RESULT FAIL: crc device not ready\n");
		return 0;
	}
	printk("crc device ready\n");
	printk(
	    "input: %zu bytes, expected CRC32_IEEE = 0x%08x\n", sizeof(fixed_buf), EXPECTED_CRC32_IEEE);

	/* 2. configure the engine for this algorithm (selects CRC_32 + size,
	 *    maps the reverse flags to the REFLECT bit, loads seed + INIT). */
	rc = crc_begin(crc_dev, &ctx);
	printk("crc_begin() rc=%d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: crc_begin rc=%d\n", rc);
		return 0;
	}

	/* 3. feed the whole fixed buffer in one update.  The 32-bit algorithms
	 *    require a multiple-of-4 length (our buffer is 16); a non-aligned
	 *    length would return -ENOTSUP. */
	rc = crc_update(crc_dev, &ctx, fixed_buf, sizeof(fixed_buf));
	printk("crc_update() rc=%d  running=0x%08x\n", rc, ctx.result);
	if (rc != 0) {
		printk("RESULT FAIL: crc_update rc=%d\n", rc);
		(void)crc_finish(crc_dev, &ctx);
		return 0;
	}

	/* 4. finalise: latches the engine CRC_OUT into ctx.result + frees the
	 *    engine lock. */
	rc = crc_finish(crc_dev, &ctx);
	printk("crc_finish() rc=%d  result=0x%08x\n", rc, ctx.result);
	if (rc != 0) {
		printk("RESULT FAIL: crc_finish rc=%d\n", rc);
		return 0;
	}

	/* --- J-Link readback window: mem32 0x48107018 (engine CRC_OUT) --- */

	/* 5. verdict: compare the hardware result against the host reference via
	 *    the portable crc_verify() helper (returns 0 only when they match and
	 *    the calc is complete). */
	rc = crc_verify(&ctx, EXPECTED_CRC32_IEEE);
	if (rc == 0) {
		printk("RESULT PASS: crc matches reference "
		       "(computed=0x%08x expected=0x%08x)\n",
		       ctx.result,
		       EXPECTED_CRC32_IEEE);
	} else {
		printk("RESULT FAIL: crc mismatch "
		       "(computed=0x%08x expected=0x%08x crc_verify rc=%d)  -- "
		       "read CRC_OUT (mem32 0x48107018) over J-Link and compare "
		       "the raw value; see the reflect/invert bench note\n",
		       ctx.result,
		       EXPECTED_CRC32_IEEE,
		       rc);
	}

	return 0;
}
