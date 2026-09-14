/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2098 (review follow-up): examples/aen/aen-evk-demo's Phase 4
 * (I/O expander) polarity round-trip verdict, pulled into its own header
 * precisely so it can be exercised here without a board -- see
 * ioexp_verdict.h's file comment for the full reasoning. Reachable via
 * this app's existing examples/aen/aen-evk-demo/src include dir (see
 * bmp581_verdict.h's comment in this app's CMakeLists.txt).
 */

#include <zephyr/ztest.h>

#include "ioexp_verdict.h"

#define TEST_MASK 0xF0u /* P4-P7, same as main.c's IOEXP_POLARITY_TEST_MASK. */

ZTEST(alp_chips, test_ioexp_polarity_healthy_round_trip_passes)
{
	/* before: P0-3 out (0), P4-7 in (1) -- the netlist-expected 0xF0.
	 * inverted: exactly the masked bits flipped. restored: back to
	 * before, byte-identical. */
	ioexp_polarity_result_t r = ioexp_polarity_check(0xF0u, 0x00u, 0xF0u, TEST_MASK);

	zassert_true(r.inversion_took_effect, "all four masked bits flipped, must register as such");
	zassert_true(r.restore_took_effect, "restored == before, must register as such");
	zassert_true(ioexp_polarity_result_pass(r), "a clean round trip must PASS");
}

ZTEST(alp_chips, test_ioexp_polarity_vacuous_restore_now_fails)
{
	/* The exact review truth table (Major 1): before=0xF3, inverted=
	 * 0x03 (invert genuinely took effect on the masked bits), restored
	 * =0x03 -- the restore WRITE ACKed over I2C but register 0x02 never
	 * actually returned to 0x00, so the chip is STILL inverted. The old
	 * mask-outside-TEST_MASK restore check missed this because it only
	 * ever compared the bits this phase never wrote (P0-P3); comparing
	 * the whole byte must catch it. */
	ioexp_polarity_result_t r = ioexp_polarity_check(0xF3u, 0x03u, 0x03u, TEST_MASK);

	zassert_true(r.inversion_took_effect, "the masked bits did flip on invert");
	zassert_false(r.restore_took_effect,
	              "restored (0x03) != before (0xF3) -- the restore write never really landed");
	zassert_false(ioexp_polarity_result_pass(r),
	              "an un-restored chip must FAIL the phase, not pass on a vacuous check");
}

ZTEST(alp_chips, test_ioexp_polarity_wedged_part_fails)
{
	/* A wedged/absent/sentinel expander returns the SAME constant byte
	 * on every read regardless of what this phase writes -- before ==
	 * inverted == restored. Neither check may pass vacuously here: the
	 * XOR-with-mask is 0 (not `mask`), so inversion_took_effect is
	 * false outright, same #2037-class guard ioexp_verdict.h's header
	 * comment describes. */
	ioexp_polarity_result_t r = ioexp_polarity_check(0x42u, 0x42u, 0x42u, TEST_MASK);

	zassert_false(r.inversion_took_effect, "an unchanging byte never flipped the masked bits");
	zassert_false(ioexp_polarity_result_pass(r), "a wedged part must FAIL, not PASS");
}

ZTEST(alp_chips, test_ioexp_polarity_live_interrupt_cancels_one_bit_still_fails_that_attempt)
{
	/* #2098 review, Major 3: P4-P7 are live sensor-interrupt lines (the
	 * ICM42670 keeps running at 100 Hz for this whole app's life). If a
	 * real interrupt edge happens to land between the `before` and
	 * `inverted` reads and cancels exactly one of the four masked bits'
	 * forced flip, this ONE attempt correctly reports it did not fully
	 * invert -- ioexp_polarity_check() stays strict per attempt; a
	 * bounded retry at the call site (main.c's
	 * IOEXP_ROUND_TRIP_ATTEMPTS), not a looser predicate here, is what
	 * tolerates the live edge across MULTIPLE attempts. before=0xF0
	 * (P4-7 all high); a full invert would give 0x00, but bit 6
	 * (P6/FSYNC) independently toggled and is still reporting 1. */
	ioexp_polarity_result_t r = ioexp_polarity_check(0xF0u, 0x40u, 0xF0u, TEST_MASK);

	zassert_false(r.inversion_took_effect,
	              "one masked bit not flipping (a live edge) must still fail THIS attempt");
	zassert_false(ioexp_polarity_result_pass(r),
	              "this attempt must FAIL outright -- retry is a call-site concern, not this "
	              "predicate's job to paper over");
}

ZTEST(alp_chips, test_ioexp_polarity_reader_never_shifts_bits_outside_mask)
{
	/* Bits outside TEST_MASK (P0-P3) must never be REQUIRED to move for
	 * inversion_took_effect -- this phase never asks the chip to invert
	 * them, so a real chip's own P0-P3 read-back is free to stay put
	 * while P4-P7 do the inverting this test checks. */
	ioexp_polarity_result_t r = ioexp_polarity_check(0x0Au, 0xFAu, 0x0Au, TEST_MASK);

	zassert_true(r.inversion_took_effect, "P4-7 flipped (0x0A->0xFA); P0-3 unchanged is fine here");
	zassert_true(r.restore_took_effect, "restored == before across the whole byte");
	zassert_true(ioexp_polarity_result_pass(r), "must PASS");
}
