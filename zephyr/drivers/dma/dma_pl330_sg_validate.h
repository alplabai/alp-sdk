/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scatter-gather chain-length validation for the vendored PL330 DMA driver
 * (see dma_pl330_alif.c for the full ADR 0017 provenance banner and the
 * #1124 divergence note). Split into its own tiny, dependency-free header
 * (<zephyr/drivers/dma.h>'s struct dma_block_config only) so
 * tests/unit/dma_pl330_sg_validate can exercise the exact chain-length check
 * dma_pl330_configure() runs, without pulling in DEVICE_MMIO or a
 * devicetree-instantiated DMA instance.
 */
#ifndef ZEPHYR_DRIVERS_DMA_DMA_PL330_SG_VALIDATE_H_
#define ZEPHYR_DRIVERS_DMA_DMA_PL330_SG_VALIDATE_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/dma.h>

/**
 * @brief Confirm a scatter-gather linked list has EXACTLY @p count entries.
 *
 * Walks at most @p count nodes starting at @p head; rejects both a list
 * shorter than @p count (a NULL next_block reached before @p count nodes
 * were visited -- the #1124 defect, which otherwise left a stale/zeroed
 * pool entry linked into the chain as a phantom block) and a list longer
 * than @p count (walking still finds a non-NULL node after @p count
 * entries -- previously silently truncated).
 *
 * Bounded by @p count itself, so this can never loop unboundedly even
 * against a cyclic list.
 *
 * @param head Head of the caller-supplied scatter-gather chain. NULL is
 *             always rejected (an empty/absent chain never matches a
 *             positive count).
 * @param count Expected number of entries (cfg->block_count, floored to 1
 *              by the caller).
 *
 * @return true iff the chain has exactly @p count entries.
 */
static inline bool dma_pl330_sg_chain_matches(const struct dma_block_config *head,
					       uint32_t count)
{
	const struct dma_block_config *walk = head;
	uint32_t visited = 0;

	while (walk != NULL && visited < count) {
		visited++;
		walk = walk->next_block;
	}

	return (visited == count) && (walk == NULL);
}

#endif /* ZEPHYR_DRIVERS_DMA_DMA_PL330_SG_VALIDATE_H_ */
