/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1124: dma_pl330_alif.c's dma_pl330_configure() trusted cfg->block_count
 * against a caller-supplied linked list without confirming the list
 * actually had that many entries. A list shorter than block_count left a
 * stale/zeroed pool entry linked into the chain as a phantom zero-address/
 * zero-size block the ISR could try to start; a longer list was silently
 * truncated. Exercised here against the exact chain-length guard
 * dma_pl330_configure() now runs, on the host, with no MMIO/devicetree
 * involved.
 */
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#include "dma_pl330_sg_validate.h"

ZTEST_SUITE(dma_pl330_sg_validate, NULL, NULL, NULL, NULL, NULL);

/* NULL head: never valid, regardless of count. */
ZTEST(dma_pl330_sg_validate, test_null_head_is_rejected)
{
	zassert_false(dma_pl330_sg_chain_matches(NULL, 1), "NULL head must reject count 1");
	zassert_false(dma_pl330_sg_chain_matches(NULL, 3), "NULL head must reject count 3");
}

/* A valid multi-block chain matching its own length. */
ZTEST(dma_pl330_sg_validate, test_valid_multi_block_chain_matches)
{
	struct dma_block_config b2 = { .next_block = NULL };
	struct dma_block_config b1 = { .next_block = &b2 };
	struct dma_block_config b0 = { .next_block = &b1 };

	zassert_true(dma_pl330_sg_chain_matches(&b0, 3), "a 3-node chain must match count 3");
	zassert_true(dma_pl330_sg_chain_matches(&b0, 1) == false,
	             "a 3-node chain must NOT match count 1 (list longer than count)");
}

/* Single-block chain, the common case. */
ZTEST(dma_pl330_sg_validate, test_single_block_chain_matches_count_1)
{
	struct dma_block_config b0 = { .next_block = NULL };

	zassert_true(dma_pl330_sg_chain_matches(&b0, 1), "a 1-node chain must match count 1");
}

/*
 * The exact #1124 defect: block_count claims 3 blocks but the caller's
 * list only has 2 -- this is what used to leave a phantom zero-address/
 * zero-size third pool entry linked into the chain. Must be rejected.
 */
ZTEST(dma_pl330_sg_validate, test_short_chain_would_have_created_phantom_block)
{
	struct dma_block_config b1 = { .next_block = NULL };
	struct dma_block_config b0 = { .next_block = &b1 };

	zassert_false(dma_pl330_sg_chain_matches(&b0, 3),
	              "a 2-node chain must not validate against block_count 3 -- "
	              "this used to leave a phantom zero-size block linked in");
}

/* A chain longer than block_count -- previously silently truncated. */
ZTEST(dma_pl330_sg_validate, test_long_chain_is_rejected_not_truncated)
{
	struct dma_block_config b2 = { .next_block = NULL };
	struct dma_block_config b1 = { .next_block = &b2 };
	struct dma_block_config b0 = { .next_block = &b1 };

	zassert_false(dma_pl330_sg_chain_matches(&b0, 2),
	              "a 3-node chain must not validate against block_count 2");
}

/* count == 0 against a non-empty chain must reject (defensive; the driver
 * itself floors count to 1 before calling this, but the helper's own
 * contract must hold regardless of caller behavior).
 */
ZTEST(dma_pl330_sg_validate, test_zero_count_against_nonempty_chain_rejects)
{
	struct dma_block_config b0 = { .next_block = NULL };

	zassert_false(dma_pl330_sg_chain_matches(&b0, 0), "count 0 must not match a non-empty chain");
}
