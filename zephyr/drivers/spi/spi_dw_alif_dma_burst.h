/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMA burst-length computation for the vendored Alif DWC_ssi SPI driver
 * (spi_dw_alif.c). Split into its own tiny, dependency-free header
 * (stddef.h/stdint.h/stdbool.h only) so tests/unit/spi_dw_alif_dma_burst can
 * exercise the burst-length reduction on the host, without pulling in
 * DEVICE_MMIO or a devicetree-instantiated SPI instance.
 *
 * spi_dw_dma_burst_for_chunk() is the burst length each direction's setup
 * function hands the PL330 dma_cfg -- the actual burst size the DMA engine
 * executes, which must divide the chunk evenly or a final partial burst
 * never crosses the FIFO watermark and the transfer hangs.
 *
 * It is NOT the value every caller writes into the DW_SSI watermark register
 * (DMATDLR / DMARDLR). DMATDLR/DMARDLR is a LOW WATERMARK -- e.g. the TX
 * request asserts once the TX FIFO holds <= DMATDLR entries -- not a burst
 * count, and the only hardware constraint tying the two together is
 * watermark + burst <= fifo_depth. A #1818 follow-up briefly programmed the
 * reduced burst into DMATDLR on the assumption the two had to match (down to
 * DMATDLR=1 for a dummy-TX chunk); that was reverted once review established
 * the watermark/burst distinction above -- forcing the PL330 to refill in
 * much smaller increments against a DW_apb_ssi master that gates SCLK on TX
 * underrun is an unproven behavioural change with no bench evidence either
 * way. See the CONFIG_SPI_DW_ALIF_DMA_MIN_LEN comment in
 * examples/aen/aen-cc3501e-bringup/prj.conf for the open question review
 * could not settle from source: whether the controller honours a
 * watermark-register write at all while SSIENR=1, which is the state
 * spi_dw_dma_setup_tx_channel() / _rx_channel() always find it in.
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
 * @brief The burst length to program into the PL330 dma_cfg source/dest
 * burst length fields for this chunk/direction.
 *
 * This is NOT necessarily the value that belongs in the DW_SSI watermark
 * register (DMATDLR/DMARDLR) too -- see this file's header comment. In
 * particular the RX caller (spi_dw_dma_setup_rx_channel()) programs
 * @c burstlen @c - @c 1 into DMARDLR, never @c burstlen itself.
 *
 * @param default_burst Preferred burst length (fifo_depth / 2).
 * @param chunk Number of items in this transfer; ignored when @p is_dummy
 *              is true.
 * @param is_dummy True for a placeholder direction: dummy_tx, reached only
 *                  when the current TX buffer's `buf` is NULL or the TX
 *                  buffers are exhausted while RX continues (an RX-only
 *                  transfer never reaches it -- spi_dw_dma_setup_tx_channel()
 *                  is then not called at all); or dummy_rx, the mirrored
 *                  RX-side case. Always moves exactly 1 item per burst
 *                  regardless of chunk.
 * @return The burst length to write, unreduced, into dma_cfg's source/dest
 *         burst length fields.
 */
static inline uint32_t spi_dw_dma_burst_for_chunk(uint32_t default_burst, size_t chunk,
						   bool is_dummy)
{
	return is_dummy ? 1u : spi_dw_dma_calculate_burst_length(default_burst, chunk);
}

#endif /* ZEPHYR_DRIVERS_SPI_SPI_DW_ALIF_DMA_BURST_H_ */
