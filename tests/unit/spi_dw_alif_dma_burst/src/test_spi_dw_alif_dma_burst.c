/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-testable coverage for spi_dw_dma_calculate_burst_length() /
 * spi_dw_dma_burst_for_chunk() (spi_dw_alif_dma_burst.h) -- the PL330
 * dma_cfg burst length spi_dw_dma_setup_tx_channel() / _rx_channel() compute
 * for a transfer chunk. The burst must divide the chunk evenly, or a final
 * partial burst never crosses the DW_SSI FIFO watermark and the transfer
 * hangs.
 *
 * This is dma_cfg's burst length only -- NOT necessarily what is written
 * into the DW_SSI watermark register (DMATDLR/DMARDLR); see
 * spi_dw_alif_dma_burst.h's file header for why the two are allowed to
 * differ, and NOTE that a dummy-TX chunk is never reached by an RX-only
 * transfer (spi_dw_dma_setup_tx_channel() is simply not called for one) --
 * it is reached only when a supplied tx_buf_set's current buffer has
 * `buf == NULL`, or its buffers are exhausted while RX continues.
 */
#include <zephyr/ztest.h>

#include "spi_dw_alif_dma_burst.h"

ZTEST_SUITE(spi_dw_alif_dma_burst, NULL, NULL, NULL, NULL, NULL);

/* A round chunk (multiple of the default burst) never triggers a reduction,
 * so the pure calculator returns the default unchanged.
 */
ZTEST(spi_dw_alif_dma_burst, test_round_chunk_keeps_default_burst)
{
	zassert_equal(spi_dw_dma_calculate_burst_length(8, 256),
	              8,
	              "256 is a multiple of 8 -- burst should stay at the default");
	zassert_equal(spi_dw_dma_calculate_burst_length(8, 64),
	              8,
	              "64 is a multiple of 8 -- burst should stay at the default");
}

/* The returned burst must always evenly divide the chunk -- the DW_SSI only
 * pulses its DMA request on a FIFO watermark crossing, so a burst that does
 * not divide the chunk exactly would leave a final partial burst that never
 * requests and hangs the transfer.
 */
ZTEST(spi_dw_alif_dma_burst, test_burst_always_divides_chunk)
{
	for (size_t chunk = 1; chunk <= 32; chunk++) {
		uint32_t burst = spi_dw_dma_calculate_burst_length(8, chunk);

		zassert_true(burst >= 1 && burst <= 8, "burst %u out of [1,8] for chunk %zu", burst, chunk);
		zassert_equal(chunk % burst, 0, "burst %u does not divide chunk %zu", burst, chunk);
	}
}

/* The exact tail shape a real OTA_WRITE leaves: ALP_CC3501E_OTA_MAX_CHUNK
 * (4092) is not a multiple of the default burst (8) -- spi_dw_dma_transceive()
 * splits it into a 4088 B aligned bulk (unaffected: 4088 % 8 == 0) plus a 4 B
 * tail on the next loop pass. This is the tail's own chunk, in isolation.
 */
ZTEST(spi_dw_alif_dma_burst, test_ota_max_chunk_tail_reduces_burst)
{
	const size_t   ota_tail_chunk = 4092 % 8; /* == 4 */
	const uint32_t default_burst  = 8;
	uint32_t       burst = spi_dw_dma_burst_for_chunk(default_burst, ota_tail_chunk, false);

	zassert_equal(ota_tail_chunk, 4, "test's own arithmetic sanity check");
	zassert_equal(burst, 4, "a 4-item tail must reduce the burst from 8 to 4");
}

/* A dummy placeholder direction always moves exactly 1 item per burst,
 * regardless of chunk or default burst.
 */
ZTEST(spi_dw_alif_dma_burst, test_dummy_forces_burst_of_one)
{
	uint32_t burst = spi_dw_dma_burst_for_chunk(8, 4096, true);

	zassert_equal(burst, 1, "a dummy TX/RX placeholder must always move 1 item per burst");
}

/* spi_dw_dma_burst_for_chunk() must route non-dummy calls through the same
 * reduction spi_dw_dma_calculate_burst_length() performs -- it is a thin
 * wrapper, not a second, divergent implementation.
 */
ZTEST(spi_dw_alif_dma_burst, test_burst_for_chunk_matches_calculate_when_not_dummy)
{
	for (size_t chunk = 1; chunk <= 32; chunk++) {
		zassert_equal(spi_dw_dma_burst_for_chunk(8, chunk, false),
		              spi_dw_dma_calculate_burst_length(8, chunk),
		              "wrapper must agree with the calculator for chunk %zu",
		              chunk);
	}
}
