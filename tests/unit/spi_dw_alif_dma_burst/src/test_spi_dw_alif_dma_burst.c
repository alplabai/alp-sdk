/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1818 follow-up: spi_dw_dma_setup_tx_channel() used to write two DIFFERENT
 * values for what should be one hardware fact -- DMATDLR got the raw
 * half-FIFO default (dw_spi_txftlr_dflt) unconditionally, while the PL330
 * dma_cfg burst length got spi_dw_dma_calculate_burst_length()'s possibly
 * SMALLER result. spi_dw_dma_burst_for_chunk() (spi_dw_alif_dma_burst.h) now
 * computes the one value both writes use.
 *
 * "old_watermark" below is literally what spi_dw_alif.c wrote into DMATDLR
 * before the fix: the unreduced default, full stop. This suite asserts that
 * for realistic inputs -- a dummy-TX (RX-only) transfer, and the exact tail
 * shape ALP_CC3501E_OTA_MAX_CHUNK (4092) leaves after the bulk/tail split in
 * spi_dw_dma_transceive() (4092 % 8 = 4, so the final chunk is 4 items) --
 * old_watermark() disagrees with the burst length actually programmed into
 * the DMA engine, while spi_dw_dma_burst_for_chunk() cannot, by construction.
 */
#include <zephyr/ztest.h>

#include "spi_dw_alif_dma_burst.h"

ZTEST_SUITE(spi_dw_alif_dma_burst, NULL, NULL, NULL, NULL, NULL);

/* What spi_dw_dma_setup_tx_channel() wrote into DMATDLR before #1818's
 * follow-up fix: the raw default, ignoring chunk/dummy entirely.
 */
static uint32_t old_watermark(uint32_t default_burst)
{
	return default_burst;
}

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

	/* This is the regression: the OLD code wrote old_watermark(default_burst)
	 * into DMATDLR regardless of chunk, which disagrees with the burst the
	 * DMA engine was actually told to move. */
	zassert_not_equal(old_watermark(default_burst),
	                  burst,
	                  "pre-fix DMATDLR value (%u) must NOT match the real burst (%u) here "
	                  "-- that mismatch is exactly what #1818's follow-up fixes",
	                  old_watermark(default_burst),
	                  burst);
}

/* The dummy-TX case (an RX-only transfer's placeholder source) always moves
 * exactly 1 item per burst, regardless of chunk -- and regardless of how
 * large the default burst is.
 */
ZTEST(spi_dw_alif_dma_burst, test_dummy_forces_burst_of_one)
{
	uint32_t burst = spi_dw_dma_burst_for_chunk(8, 4096, true);

	zassert_equal(burst, 1, "a dummy TX/RX placeholder must always move 1 item per burst");

	/* Same regression shape as the OTA tail case: the OLD code wrote the raw
	 * default into DMATDLR even for a dummy transfer. */
	zassert_not_equal(old_watermark(8),
	                  burst,
	                  "pre-fix DMATDLR value (8) must NOT match the dummy-forced burst (1)");
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
