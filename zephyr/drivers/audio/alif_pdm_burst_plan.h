/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * IRQ-burst-to-slab-block split planning for the vendored Alif PDM driver
 * (see alif_pdm.c for the full ADR 0017 provenance banner and the #1122
 * divergence note). Split into its own tiny, dependency-free header
 * (stdint/stddef only) so tests/unit/alif_pdm_burst_plan can exercise the
 * exact split algorithm the ISR runs, without pulling in DEVICE_MMIO, the
 * k_mem_slab allocator, or a devicetree-instantiated PDM instance.
 */
#ifndef ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_BURST_PLAN_H_
#define ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_BURST_PLAN_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Plan how one PDM IRQ burst is split across slab block(s).
 *
 * The PDM ISR may receive up to a 128-byte burst (MAX_DATA_ITEMS *
 * MAX_NUM_CHANNELS * sizeof(uint16_t)) in a single call, which can exceed
 * the space left in the block already in progress -- or even exceed the
 * configured block_size itself. Rather than trusting one replacement block
 * to hold the whole remainder (#1122, which corrupted adjacent slab
 * memory), this plans a sequence of chunk sizes: the first chunk (which may
 * be 0) fills out the block already in progress; every chunk after that
 * starts a fresh block and is never larger than block_size.
 *
 * @param first_block_available Bytes of free space at the *end* of the
 *                               block already in progress (0 if none is in
 *                               progress, or it happens to already be
 *                               exactly full).
 * @param data_bytes Total size of this IRQ burst, in bytes. May be 0 (no
 *                    data -- yields a single 0-byte chunk).
 * @param block_size Configured slab block size, in bytes. MUST be > 0.
 * @param chunks_out Array to receive each chunk's size, in copy order.
 *                    MUST NOT be NULL.
 * @param max_chunks Capacity of @p chunks_out.
 *
 * @return The number of chunks written to @p chunks_out (always >= 1) on
 *         success. Returns 0 if @p max_chunks was too small to hold the
 *         whole plan -- callers MUST treat that as "cannot safely split
 *         this burst" and drop the burst rather than guess, never write
 *         partial chunks.
 */
static inline size_t pdm_plan_burst_chunks(uint32_t first_block_available, uint32_t data_bytes,
					    uint32_t block_size, uint32_t *chunks_out,
					    size_t max_chunks)
{
	size_t n;
	uint32_t copied;
	uint32_t first;

	if (max_chunks == 0) {
		return 0;
	}

	first = (first_block_available < data_bytes) ? first_block_available : data_bytes;
	chunks_out[0] = first;
	copied = first;
	n = 1;

	while (copied < data_bytes) {
		uint32_t remaining = data_bytes - copied;
		uint32_t chunk = (remaining < block_size) ? remaining : block_size;

		if (n >= max_chunks) {
			return 0;
		}

		chunks_out[n++] = chunk;
		copied += chunk;
	}

	return n;
}

#endif /* ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_BURST_PLAN_H_ */
