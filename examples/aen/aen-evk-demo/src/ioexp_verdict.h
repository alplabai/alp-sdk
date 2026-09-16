/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2098: examples/aen/aen-evk-demo's Phase 4 (I/O expander) polarity
 * round-trip verdict, pulled out of main.c into its own header so it is
 * unit-testable without a board -- same pattern as bmp581_verdict.h,
 * cc3501e_link_verdict.h and sound_verdict.h in this directory.
 *
 * ONE ROUND-TRIP ATTEMPT: read `before`, write the TCAL9538's
 * polarity-inversion register (0x02) with `mask` set, read `inverted`,
 * write the register back to 0x00, read `restored`.
 * ioexp_polarity_check() judges exactly that one attempt -- it takes no
 * I2C return codes, so a caller still has to gate on its own transfer
 * return codes first (a genuine ALP_OK with a byte that happens to look
 * plausible is not proof a transfer landed anything; see the
 * sentinel-init idiom at this phase's call site in main.c).
 *
 * TWO WAYS ONE ATTEMPT CAN FAIL, both real, both worth a name:
 *
 *   - inversion never took effect: `mask`'s bits didn't flip between
 *     `before` and `inverted` -- a wedged expander returning one
 *     constant byte on every read hits this every time (#2037).
 *   - restore never took effect: `restored` isn't `before` again.
 *     #2098 found this checked with a mask that covered ONLY the bits
 *     the phase never wrote (P0-P3) -- vacuously satisfied by a chip
 *     that ACKs the restore write but never actually clears the
 *     register, because the bits that were actually inverted (`mask`)
 *     were never compared. Comparing the WHOLE byte catches that: see
 *     test_ioexp_polarity_vacuous_restore_now_fails in
 *     tests/zephyr/chips/src/test_ioexp_verdict.c for the exact
 *     regression case.
 *
 * WHY A SINGLE FAILED ATTEMPT IS NOT AUTOMATICALLY A FAULT, AND WHY THIS
 * HEADER DOES NOT TRY TO FIX THAT ITSELF. `mask` covers P4-P7, the four
 * live sensor-interrupt lines (ICM42670 INT1/INT2/FSYNC, BMP581 INT1) --
 * phase 2 leaves the ICM42670 running accelerometer output at 100 Hz for
 * the rest of this app's run (it is never deinitialised), so a real
 * interrupt edge landing between two of this attempt's reads can make a
 * healthy expander look like it failed the round trip. This function
 * does not know that and does not compensate for it: that is a
 * call-site policy (a bounded retry -- see main.c's
 * IOEXP_ROUND_TRIP_ATTEMPTS), not a predicate weakened here. Loosening
 * this function to tolerate a stray bit would also let a genuinely
 * wedged expander -- which "loosens" the exact same way -- pass.
 */
#ifndef ALP_EVK_DEMO_IOEXP_VERDICT_H
#define ALP_EVK_DEMO_IOEXP_VERDICT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool inversion_took_effect;
	bool restore_took_effect;
} ioexp_polarity_result_t;

/* `mask` is the set of bits this attempt asked the chip to invert (and
 * only those -- see main.c's IOEXP_POLARITY_TEST_MASK for why P0-P3 are
 * never in it). */
static inline ioexp_polarity_result_t
ioexp_polarity_check(uint8_t before, uint8_t inverted, uint8_t restored, uint8_t mask)
{
	ioexp_polarity_result_t r;
	r.inversion_took_effect = (((uint8_t)(inverted ^ before)) & mask) == mask;
	r.restore_took_effect   = (restored == before);
	return r;
}

static inline bool ioexp_polarity_result_pass(ioexp_polarity_result_t r)
{
	return r.inversion_took_effect && r.restore_took_effect;
}

#endif /* ALP_EVK_DEMO_IOEXP_VERDICT_H */
