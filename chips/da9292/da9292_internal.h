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
 * #757 regression coverage: the CH2_PG poll loop in
 * da9292_v2n_m1_enable_deepx_rail() decides, once per pass, whether
 * the remaining poll budget covers one more `poll_us` slice. Factored
 * out as a pure function so the boundary arithmetic (in particular
 * the `timeout_us == UINT32_MAX` case) is directly testable without
 * needing the I2C test double to sequence per-register responses
 * (its canned response is an address-keyed snapshot, not a queue --
 * see include-testing/alp/testing/i2c.h -- so it cannot make
 * da9292_get_status() report "not yet powered good" on some polls and
 * "powered good" on others).
 *
 * Returns true if the caller should poll again (and has already
 * decremented *remaining_us by poll_us), false if the budget is
 * exhausted (*remaining_us left unmodified -- caller returns
 * ALP_ERR_TIMEOUT). Strictly decreasing every call that returns true,
 * so a caller looping on this can never spin forever regardless of
 * the initial *remaining_us value (including UINT32_MAX).
 */
bool da9292_poll_budget_step(uint32_t *remaining_us, uint32_t poll_us);

#endif /* DA9292_INTERNAL_H */
