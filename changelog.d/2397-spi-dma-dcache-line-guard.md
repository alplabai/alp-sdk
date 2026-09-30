### Fixed — Alif SPI DMA no longer corrupts memory next to an unaligned RX buffer with the D-cache on (#2397)

`spi_dw_alif`'s DMA path invalidates each RX destination after the transfer
(#1830). Cache maintenance works on whole lines, so an RX buffer that started
or ended mid-line also threw away any not-yet-written-back CPU store to the
bytes sharing that line. On the CC3501E bridge those bytes were fields of the
`cc3501e_t` around `rx_scratch[]`: with `CONFIG_DCACHE=y` and
`CONFIG_SPI_DW_ALIF_USE_DMA=y` the bridge never answered PING and the core
then took an MPU fault through a clobbered pointer.

`spi_dw_should_dma()` now takes the DMA path only when every RX buffer's
start and length are multiples of `CONFIG_DCACHE_LINE_SIZE`; anything else
uses the PIO path. Builds without `CONFIG_DCACHE` are unchanged.

Verified on E1M-AEN803 2026W36-0009: `aen-cc3501e-socket-throughput` built
with the #2397 DMA overlay and `CONFIG_DCACHE=y` now prints
`STEP 2: PING ok after 1 attempt` and starts its STREAM_WRITE sweep, with no
fault.
