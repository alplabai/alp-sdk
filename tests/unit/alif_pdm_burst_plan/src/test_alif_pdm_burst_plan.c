/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1122: alif_pdm.c's ISR slab rollover allocated exactly one replacement
 * block and copied the WHOLE remainder of an IRQ burst into it, without
 * checking the block was large enough -- a valid 32-byte block could
 * receive up to 96 bytes after rollover, corrupting adjacent slab memory.
 * Exercised here against the exact split-planning algorithm the ISR now
 * calls, on the host, with no MMIO/k_mem_slab/devicetree involved.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

#include "alif_pdm_burst_plan.h"

ZTEST_SUITE(alif_pdm_burst_plan, NULL, NULL, NULL, NULL, NULL);

#define MAX_DATA_ITEMS_TEST   8u
#define MAX_NUM_CHANNELS_TEST 8u
/* The real hardware ceiling: MAX_DATA_ITEMS * MAX_NUM_CHANNELS * sizeof(uint16_t). */
#define MAX_BURST_BYTES (MAX_DATA_ITEMS_TEST * MAX_NUM_CHANNELS_TEST * 2u)

/* The exact repro from the issue's Evidence section: a valid 32-byte block
 * must never receive more than 32 bytes in a single memcpy, even when a
 * full 128-byte burst arrives with the block already empty.
 */
ZTEST(alif_pdm_burst_plan, test_32_byte_block_never_overruns_on_full_burst)
{
	uint32_t chunks[MAX_BURST_BYTES + 1];
	size_t   n   = pdm_plan_burst_chunks(32, MAX_BURST_BYTES, 32, chunks, ARRAY_SIZE(chunks));
	uint32_t sum = 0;

	zassert_true(n > 0, "plan must succeed for a realistic block size");
	for (size_t i = 0; i < n; i++) {
		zassert_true(
		    chunks[i] <= 32, "chunk %zu (%u bytes) overruns the 32-byte block", i, chunks[i]);
		sum += chunks[i];
	}
	zassert_equal(sum, MAX_BURST_BYTES, "chunks must account for every byte of the burst");
}

/* block_size below, at, and above the hardware burst ceiling (acceptance
 * criteria): in every case, no chunk after the first may exceed
 * block_size, and the chunks must sum to data_bytes exactly.
 */
struct plan_case {
	const char *name;
	uint32_t    first_available;
	uint32_t    data_bytes;
	uint32_t    block_size;
};

static const struct plan_case cases[] = {
	{ "block_size below burst ceiling, fresh block", MAX_BURST_BYTES, MAX_BURST_BYTES, 16 },
	{ "block_size below burst ceiling, mid-block", 5, MAX_BURST_BYTES, 16 },
	{ "block_size equal to burst ceiling", MAX_BURST_BYTES, MAX_BURST_BYTES, MAX_BURST_BYTES },
	{ "block_size above burst ceiling", 256, MAX_BURST_BYTES, 256 },
	{ "current block already exactly full", 0, 50, 20 },
	{ "zero-length burst", MAX_BURST_BYTES, 0, 16 },
	{ "block_size of 1 (pathological but must still not overrun)", 1, 10, 1 },
};

ZTEST(alif_pdm_burst_plan, test_plan_table_never_overruns)
{
	for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
		const struct plan_case *tc = &cases[c];
		uint32_t                chunks[MAX_BURST_BYTES + 2];
		size_t                  n = pdm_plan_burst_chunks(
		    tc->first_available, tc->data_bytes, tc->block_size, chunks, ARRAY_SIZE(chunks));
		uint32_t sum = 0;

		zassert_true(n > 0, "case '%s': plan must succeed", tc->name);

		/* The first chunk fills at most the space already free in the
		 * in-progress block.
		 */
		zassert_true(chunks[0] <= tc->first_available,
		             "case '%s': first chunk (%u) exceeds first_available (%u)",
		             tc->name,
		             chunks[0],
		             tc->first_available);

		for (size_t i = 0; i < n; i++) {
			sum += chunks[i];
			if (i > 0) {
				/* Every chunk after the first starts a FRESH
				 * block -- this is the invariant #1122 broke.
				 */
				zassert_true(chunks[i] <= tc->block_size,
				             "case '%s': chunk %zu (%u bytes) overruns "
				             "block_size (%u)",
				             tc->name,
				             i,
				             chunks[i],
				             tc->block_size);
			}
			/* Only the very last chunk may be a partial (< block_size)
			 * fresh-block chunk; every fresh-block chunk before it must
			 * be exactly block_size (otherwise progress wasn't maximal
			 * and the plan would need more chunks than necessary).
			 */
			if (i > 0 && i + 1 < n) {
				zassert_equal(chunks[i],
				              tc->block_size,
				              "case '%s': non-final chunk %zu (%u) should "
				              "fill the whole block (%u)",
				              tc->name,
				              i,
				              chunks[i],
				              tc->block_size);
			}
		}

		zassert_equal(
		    sum, tc->data_bytes, "case '%s': chunks must sum to data_bytes exactly", tc->name);
	}
}

/* max_chunks too small to hold the plan must fail closed (0), never write a
 * partial/truncated plan the caller could act on.
 */
ZTEST(alif_pdm_burst_plan, test_max_chunks_too_small_fails_closed)
{
	uint32_t chunks[2];
	size_t   n = pdm_plan_burst_chunks(0, 100, 10, chunks, ARRAY_SIZE(chunks));

	zassert_equal(n, 0, "a plan needing more than max_chunks entries must return 0");
}
