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

/**
 * @brief Confirm EVERY block of a scatter-gather chain uses an address
 *        adjustment the PL330 driver implements.
 *
 * The PL330 CCR has a single increment bit per side, so only
 * DMA_ADDR_ADJ_INCREMENT and DMA_ADDR_ADJ_NO_CHANGE are expressible;
 * DMA_ADDR_ADJ_DECREMENT would otherwise be silently run as "no change".
 * start_block() copies the adjustment per block, so a later block carrying
 * DECREMENT must be rejected at configure() time, not only the head block.
 *
 * @param head Head of the (already length-validated) chain.
 * @param count Number of entries to inspect.
 *
 * @return true iff every inspected block's source and destination
 *         adjustment is INCREMENT or NO_CHANGE.
 */
static inline bool dma_pl330_sg_addr_adj_supported(const struct dma_block_config *head,
						   uint32_t count)
{
	const struct dma_block_config *walk = head;

	for (uint32_t i = 0; i < count && walk != NULL; i++, walk = walk->next_block) {
		if ((walk->source_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
		     walk->source_addr_adj != DMA_ADDR_ADJ_NO_CHANGE) ||
		    (walk->dest_addr_adj != DMA_ADDR_ADJ_INCREMENT &&
		     walk->dest_addr_adj != DMA_ADDR_ADJ_NO_CHANGE)) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Split a transfer of @p size bytes into the chunk one DMAGO can move
 *        and the remainder that must wait for that chunk to complete.
 *
 * @param size Bytes still to move.
 * @param max_chunk Largest transfer one microcode run can express.
 * @param[out] chunk Bytes to start now (min(size, max_chunk)).
 *
 * @return Bytes left over after @p chunk.
 */
static inline uint32_t dma_pl330_chunk_split(uint32_t size, uint32_t max_chunk, uint32_t *chunk)
{
	*chunk = (size > max_chunk) ? max_chunk : size;

	return size - *chunk;
}

/**
 * @brief Address the next chunk of a split block starts at on one side.
 *
 * Only an INCREMENT side walks through memory; a NO_CHANGE side is a FIFO or
 * data register (M2P / P2M) and must stay put, otherwise chunk 2 would
 * address register + chunk bytes.
 *
 * @param addr Address the chunk just moved started at.
 * @param chunk Bytes that chunk moved.
 * @param adj Zephyr DMA_ADDR_ADJ_* of this side.
 *
 * @return Address for the following chunk.
 */
static inline uint64_t dma_pl330_next_chunk_addr(uint64_t addr, uint32_t chunk, uint32_t adj)
{
	return (adj == DMA_ADDR_ADJ_INCREMENT) ? addr + chunk : addr;
}

#endif /* ZEPHYR_DRIVERS_DMA_DMA_PL330_SG_VALIDATE_H_ */
