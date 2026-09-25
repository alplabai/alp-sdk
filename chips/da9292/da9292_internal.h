/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private declarations shared between da9292.c and its ztest coverage.
 * Not installed, not part of the public API -- <alp/chips/da9292.h> is
 * the public surface.
 */

#ifndef DA9292_INTERNAL_H
#define DA9292_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * #757 regression coverage: every poll loop in da9292_ch2_sequence()
 * (the DEEPX_PWR_EN_REQ wait and the CH2_PG wait) decides, once per
 * pass, whether the remaining budget covers one more `poll` slice.
 * Factored out as a pure function so the boundary arithmetic (in
 * particular a UINT32_MAX budget) is directly testable without the I2C
 * test double having to sequence per-register responses (its canned
 * response is an address-keyed snapshot, not a queue -- see
 * include-testing/alp/testing/i2c.h).
 *
 * Returns true if the caller should poll again (and has already
 * decremented *remaining by poll), false if the budget is exhausted
 * (*remaining left unmodified -- the caller times out). Strictly
 * decreasing every call that returns true, so a loop on this can never
 * spin forever whatever the initial *remaining (including UINT32_MAX).
 * Unit-agnostic: the sequence counts milliseconds.
 */
bool da9292_poll_budget_step(uint32_t *remaining_us, uint32_t poll_us);

#endif /* DA9292_INTERNAL_H */
