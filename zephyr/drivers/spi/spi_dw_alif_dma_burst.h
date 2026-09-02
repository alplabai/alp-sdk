/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMA burst-length computation for the vendored Alif DWC_ssi SPI driver
 * (spi_dw_alif.c). Split into its own tiny, dependency-free header
 * (stddef.h/stdint.h/stdbool.h only) so tests/unit/spi_dw_alif_dma_burst can
 * exercise the exact value spi_dw_dma_setup_tx_channel() /
 * spi_dw_dma_setup_rx_channel() program into BOTH the DW_SSI DMA watermark
 * register (DMATDLR / DMARDLR) and the PL330 dma_cfg burst length, without
 * pulling in DEVICE_MMIO or a devicetree-instantiated SPI instance.
 *
 * #1818 follow-up (CONFIG_SPI_DW_ALIF_DMA_MIN_LEN staying load-bearing): the
 * TX side used to compute these as TWO INDEPENDENT values -- DMATDLR got the
 * raw half-FIFO default unconditionally, while dma_cfg's burst length got a
 * possibly SMALLER value for a dummy-TX (RX-only) transfer or a chunk that
 * does not divide the default burst evenly (ALP_CC3501E_OTA_MAX_CHUNK=4092
 * is exactly that shape: 4092 % 8 != 0). Routing both writes through one
 * function makes them the same value BY CONSTRUCTION, matching the RX side,
 * which already computed burstlen once and reused it for both. NOT shown to
 * be the #1818 cause on its own -- see the long comment at the TX call site
 * in spi_dw_alif.c for why -- but a genuine self-consistency defect
 * regardless: the register programmed must match what the DMA engine was
 * actually configured to move.
 */
#ifndef ZEPHYR_DRIVERS_SPI_SPI_DW_ALIF_DMA_BURST_H_
#define ZEPHYR_DRIVERS_SPI_SPI_DW_ALIF_DMA_BURST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Largest burst (<= @p default_burst) that divides @p chunk evenly.
 *
 * The DW_SSI only pulses its DMA request line on a FIFO watermark crossing,
 * so a final partial burst would never request and the transfer would hang
 * -- the burst length given to the PL330 must divide the chunk exactly.
 * Falls all the way to 1 (one DMA transaction per item) if nothing else
 * divides.
 *
 * @param default_burst Preferred burst length (fifo_depth / 2). 0 is
 *                       treated as 1.
 * @param chunk Number of items (words) in this transfer. Callers never pass
 *              0 here (the transceive loop skips zero-length chunks first).
 * @return A value in [1, default_burst] (or exactly 1 if @p default_burst
 *         was 0) that divides @p chunk exactly.
 */
static inline uint32_t spi_dw_dma_calculate_burst_length(uint32_t default_burst, size_t chunk)
{
	uint32_t burst_length = default_burst ? default_burst : 1;

	while (chunk % burst_length) {
		burst_length--;
	}

	return burst_length;
}

/**
 * @brief The ONE burst length a DMA channel setup must program into both its
 * DW_SSI watermark register and the PL330 dma_cfg, so the two can never
 * diverge.
 *
 * @param default_burst Preferred burst length (fifo_depth / 2).
 * @param chunk Number of items in this transfer; ignored when @p is_dummy
 *              is true.
 * @param is_dummy True for a placeholder direction (spi_dw_dma_transceive()'s
 *                  dummy_tx for an RX-only transfer, or dummy_rx for a
 *                  TX-only one), which always moves exactly 1 item per burst
 *                  regardless of chunk.
 * @return The burst length to write, unreduced, into the watermark register
 *         and into dma_cfg's source/dest burst length fields.
 */
static inline uint32_t spi_dw_dma_burst_for_chunk(uint32_t default_burst, size_t chunk,
						   bool is_dummy)
{
	return is_dummy ? 1u : spi_dw_dma_calculate_burst_length(default_burst, chunk);
}

#endif /* ZEPHYR_DRIVERS_SPI_SPI_DW_ALIF_DMA_BURST_H_ */
