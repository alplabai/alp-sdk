/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * AEN801 CRC engine bench check -- exercises the Alif Ensemble E8 CRC block via
 * the alp-sdk clean-room crc_alif driver (ADR 0017 Tier-1.5, compatible with the
 * upstream Zephyr CRC class API).
 *
 * What this proves, and why each case is here:
 *
 *   1. CRC32_IEEE over a 16-byte buffer.  The original case: whole 32-bit words,
 *      canonical reflected CRC-32, checked against zlib.  It exercises the
 *      BYTE_SWAP | BIT_SWAP input path and the REFLECT | INVERT output path.
 *
 *   2. CRC8_CCITT and 3./4. the two CRC-16 variants over the SAME buffer, with
 *      seed 0 and NO reflect/invert -- the plain MSB-first form, so the host
 *      reference is unambiguous.  These are the cases that caught #1832: the
 *      driver stored the raw 32-bit CRC_OUT into ctx->result, and upstream
 *      crc_verify() compares it as a full uint32_t, so a CORRECT CRC-8 was
 *      rejected with -EPERM whenever the engine left anything in CRC_OUT[31:8].
 *      Each case prints BOTH the masked ctx.result and the raw CRC_OUT register,
 *      so the residue that used to break verification is visible in the log
 *      rather than inferred.
 *
 * HWRM 15.2.4 step 3 states only WHERE the result sits ("the lowest 8-bits", "the
 * lowest 16-bits"); it does not promise the bits above read back as zero.  That
 * is the whole defect.
 *
 * Host references (Python, over the same 16 bytes):
 *
 *     >>> import zlib
 *     >>> zlib.crc32(bytes([0x10,0x20,0x30,0x40, 0x50,0x60,0x70,0x80,
 *     ...                   0x90,0xA0,0xB0,0xC0, 0xD0,0xE0,0xF0,0x00]))
 *     0x684FC31C
 *
 *   plus a textbook MSB-first bit-at-a-time CRC for the narrow widths:
 *     poly 0x07   width 8   init 0 -> 0x24
 *     poly 0x1021 width 16  init 0 -> 0x0DA7   (CRC16_CCITT)
 *     poly 0x8005 width 16  init 0 -> 0x2038   (CRC16)
 *
 * The CRC-32 mapping is subtle and was bench-derived: canonical CRC-32 needs
 * input bit AND byte reflection plus a final one's-complement, so the driver
 * couples CRC_INVERT to REVERSE_OUTPUT for the 32-bit reflected types.  If the
 * printed CRC-32 does NOT equal 0x684FC31C, read CRC_OUT (mem32 0x48107018) over
 * J-Link and compare the raw engine value before any post-step.
 *
 * Console: this is a UART-console build (board default); every line below lands
 * on UART5 / the labgrid `console` resource.  Verdict lines are 'RESULT PASS:' /
 * 'RESULT FAIL:'.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/sys_io.h>

/* The CRC device is the "alif,crc" node (crc0@48107000); the crc_alif driver
 * binds it directly. */
#define CRC_NODE DT_NODELABEL(crc0)

/* Engine result register, read raw so the log shows the bits ABOVE the
 * algorithm width -- the residue that made a correct narrow CRC fail
 * verification before #1832.  base 0x48107000 + CRC_OUT 0x18; see the overlay. */
#define CRC_OUT_REG 0x48107018U

/* Fixed input buffer: 16 bytes, a multiple of 4 so the 32-bit data-input
 * register path (CRC32_IEEE / CRC32_C) consumes whole words with no remainder.
 * Keeping it const + file-scope makes it a deterministic compile-time vector. */
static const uint8_t fixed_buf[16] = {
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00,
};

static const struct device *const crc_dev = DEVICE_DT_GET(CRC_NODE);

/*
 * Run one algorithm over fixed_buf and report.  Returns true when the hardware
 * result matches the host reference via crc_verify().
 *
 * seed/reversed are per-case because the narrow algorithms are checked in their
 * plain MSB-first form (seed 0, no reflect/invert) while CRC-32 needs the
 * canonical reflected form.
 */
static bool run_case(const char   *name,
                     enum crc_type type,
                     uint32_t      poly,
                     uint32_t      seed,
                     uint8_t       reversed,
                     uint32_t      expected)
{
	struct crc_ctx ctx = {
		.type       = type,
		.polynomial = poly,
		.seed       = seed,
		.reversed   = reversed,
	};
	uint32_t raw;
	int      rc;

	printk("\n-- %s: expected 0x%08x --\n", name, expected);

	rc = crc_begin(crc_dev, &ctx);
	if (rc != 0) {
		printk("RESULT FAIL: %s crc_begin rc=%d\n", name, rc);
		return false;
	}

	rc = crc_update(crc_dev, &ctx, fixed_buf, sizeof(fixed_buf));
	if (rc != 0) {
		printk("RESULT FAIL: %s crc_update rc=%d\n", name, rc);
		(void)crc_finish(crc_dev, &ctx);
		return false;
	}

	/* Read the engine register BEFORE crc_finish() releases the lock, so the
	 * raw value belongs to this calculation. */
	raw = sys_read32(CRC_OUT_REG);

	rc = crc_finish(crc_dev, &ctx);
	if (rc != 0) {
		printk("RESULT FAIL: %s crc_finish rc=%d\n", name, rc);
		return false;
	}

	printk("%s: ctx.result=0x%08x  raw CRC_OUT=0x%08x\n", name, ctx.result, raw);

	rc = crc_verify(&ctx, expected);
	if (rc != 0) {
		printk("RESULT FAIL: %s mismatch (computed=0x%08x expected=0x%08x "
		       "crc_verify rc=%d)\n",
		       name,
		       ctx.result,
		       expected,
		       rc);
		return false;
	}

	printk("RESULT PASS: %s matches reference (0x%08x)\n", name, ctx.result);
	return true;
}

int main(void)
{
	bool all_ok = true;

	printk("\n=== AEN801 CRC engine bench (crc_alif / crc0@48107000) ===\n");

	/* If the CRC node did not instantiate a device the build would have failed
	 * at link (undefined __device_dts_ord_*), so reaching here means the device
	 * object exists -- check it is ready. */
	if (!device_is_ready(crc_dev)) {
		printk("RESULT FAIL: crc device not ready\n");
		return 0;
	}
	printk("crc device ready; input: %zu bytes\n", sizeof(fixed_buf));

	/* Canonical reflected CRC-32/IEEE: REVERSE_INPUT -> BYTE_SWAP|BIT_SWAP,
	 * REVERSE_OUTPUT -> REFLECT|INVERT, seed 0xFFFFFFFF. */
	all_ok &= run_case("CRC32_IEEE",
	                   CRC32_IEEE,
	                   CRC32_IEEE_POLY,
	                   CRC32_IEEE_INIT_VAL,
	                   CRC_FLAG_REVERSE_INPUT | CRC_FLAG_REVERSE_OUTPUT,
	                   0x684FC31CU);

	/* Narrow widths, plain MSB-first: seed 0, no reflect, no invert.  These are
	 * the #1832 cases -- watch ctx.result vs raw CRC_OUT in the log. */
	all_ok &= run_case("CRC8_CCITT", CRC8_CCITT, CRC8_POLY, 0U, 0U, 0x24U);
	all_ok &= run_case("CRC16_CCITT", CRC16_CCITT, CRC16_CCITT_POLY, 0U, 0U, 0x0DA7U);
	all_ok &= run_case("CRC16", CRC16, CRC16_POLY, 0U, 0U, 0x2038U);

	printk("\n%s: CRC regcheck -- all four algorithms\n", all_ok ? "RESULT PASS" : "RESULT FAIL");

	return 0;
}
