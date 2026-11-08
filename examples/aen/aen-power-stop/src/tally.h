/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * The running pass/fail totals of a STOP run, packed into ONE byte so they can ride in the same
 * retained cell as the cycle counter (see main.c).  Every STOP wake is a cold boot, so a plain
 * variable would restart at 0 on each wake and SUMMARY would only describe the last boot.
 *
 *   bits 7..3  pass total (saturates at 31)
 *   bits 2..0  fail total (saturates at 7; any non-zero value is already a failed run)
 */
#ifndef AEN_POWER_STOP_TALLY_H
#define AEN_POWER_STOP_TALLY_H

#include <stdint.h>

#define TALLY_PASS_MAX 31u
#define TALLY_FAIL_MAX 7u

static inline uint8_t tally_pack(unsigned pass, unsigned fail)
{
	if (pass > TALLY_PASS_MAX) {
		pass = TALLY_PASS_MAX;
	}
	if (fail > TALLY_FAIL_MAX) {
		fail = TALLY_FAIL_MAX;
	}
	return (uint8_t)((pass << 3) | fail);
}

static inline void tally_unpack(uint8_t t, unsigned *pass, unsigned *fail)
{
	*pass = t >> 3;
	*fail = t & TALLY_FAIL_MAX;
}

#endif /* AEN_POWER_STOP_TALLY_H */
