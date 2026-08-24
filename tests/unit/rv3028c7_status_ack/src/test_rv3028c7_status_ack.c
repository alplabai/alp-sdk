/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1623: rv3028c7_dispatch_irq() used to write STATUS = 0x00
 * unconditionally, clearing one-shot flags (ALARM/EXT_EVENT/BSF) that
 * latch between the STATUS read and the acknowledge write -- silently
 * losing the event.  This tests the extracted pure computation
 * (rv3028c7_status_ack_value(), chips/rv3028c7/rv3028c7_status_ack.h)
 * that must instead write back only the bits actually observed.
 *
 * Host-side (native_sim) -- the header is dependency-free, no I2C bus,
 * no fake backend needed (none exists for this chip; see #1623).
 */
#include <stdint.h>

#include <zephyr/ztest.h>

#include "rv3028c7_status_ack.h"

/* STATUS bit positions, mirrored from chips/rv3028c7/rv3028c7.c so this
 * test doesn't need the whole driver TU. */
#define STATUS_PORF 0x01u
#define STATUS_EVF  0x02u
#define STATUS_AF   0x04u
#define STATUS_TF   0x08u
#define STATUS_UF   0x10u
#define STATUS_BSF  0x20u
#define STATUS_CLKF 0x40u

ZTEST_SUITE(rv3028c7_status_ack, NULL, NULL, NULL, NULL, NULL);

struct ack_case {
	const char *name;
	uint8_t     status;
	uint8_t     want_ack;
};

static const struct ack_case cases[] = {
	/* Single recurring source observed -- only its bit clears. */
	{ "UF only", STATUS_UF, (uint8_t)(~STATUS_UF & 0x7Fu) },

	/* Single one-shot source observed alone. */
	{ "AF only", STATUS_AF, (uint8_t)(~STATUS_AF & 0x7Fu) },

	/* Both observed together -- both bits clear, nothing else touched. */
	{ "UF + AF observed together",
	  (uint8_t)(STATUS_UF | STATUS_AF),
	  (uint8_t)(~(STATUS_UF | STATUS_AF) & 0x7Fu) },

	/* Every real source observed -- everything clears, bit 7 (reserved)
     * stays 0. */
	{ "all seven sources observed",
	  (uint8_t)(STATUS_PORF | STATUS_EVF | STATUS_AF | STATUS_TF | STATUS_UF | STATUS_BSF |
	            STATUS_CLKF),
	  0x00u },

	/* Nothing observed -- ack write must still be all-1s outside the
     * reserved bit (would only be reached if a caller invoked this
     * with status == 0, which the driver itself does not do). */
	{ "nothing observed, bit 7 stays 0", 0x00u, 0x7Fu },
};

ZTEST(rv3028c7_status_ack, test_ack_value_clears_only_observed_bits)
{
	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		uint8_t got = rv3028c7_status_ack_value(cases[i].status);
		zassert_equal(got,
		              cases[i].want_ack,
		              "case '%s': status=0x%02x got=0x%02x want=0x%02x",
		              cases[i].name,
		              cases[i].status,
		              got,
		              cases[i].want_ack);
	}
}

ZTEST(rv3028c7_status_ack, test_reserved_bit7_always_written_zero)
{
	/* Bit 7 is reserved; the ack write must never set it regardless
     * of what STATUS reported (hardware only ever sets bits 0-6). */
	for (unsigned status = 0; status <= 0x7Fu; ++status) {
		uint8_t ack = rv3028c7_status_ack_value((uint8_t)status);
		zassert_equal(ack & 0x80u, 0u, "status=0x%02x ack=0x%02x set reserved bit 7", status, ack);
	}
}

ZTEST(rv3028c7_status_ack, test_a_one_shot_source_not_observed_survives_the_ack)
{
	/* This is the exact regression from #1623: STATUS read shows only
     * the recurring UF bit; ALARM (AF) has not latched yet at read
     * time, so it must NOT be cleared by the ack write -- its bit in
     * the write-back value must be 1 (untouched), not 0. Before the
     * fix this function returned 0x00 unconditionally, which clears
     * AF's bit (0) regardless -- this assertion is what a 0x00
     * implementation fails. */
	uint8_t ack = rv3028c7_status_ack_value(STATUS_UF);
	zassert_not_equal(ack & STATUS_AF,
	                  0u,
	                  "AF bit was cleared by the ack write though AF was never observed "
	                  "(status=0x%02x, ack=0x%02x) -- a flag that latches during dispatch "
	                  "would be silently swallowed",
	                  STATUS_UF,
	                  ack);
}
