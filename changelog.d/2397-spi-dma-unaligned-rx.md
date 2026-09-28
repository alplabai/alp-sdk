### Fixed — Alif SPI: DMA with the D-cache on no longer corrupts memory next to an unaligned RX buffer (#2397)

`spi_dw_alif` invalidates a DMA RX destination by cache line, before and
after each transfer (#1830). When the buffer shares its first or last line
with other data, any CPU write to that neighbour that has not been cleaned
yet is thrown away. On the CC3501E bridge, whose `rx_scratch` sits inline in
its context struct, this meant that with D-cache on and
`CONFIG_SPI_DW_ALIF_USE_DMA=y` the first PING was never answered, followed
by an MPU fault at `0x80000000` in interrupt context.

`spi_dw_should_dma()` now takes the DMA path only when every RX buffer's
start and length are both cache-line aligned. Any other transfer uses the
polled path, which is always correct. Builds without `CONFIG_DCACHE` are
unchanged.

Bench-verified on an E1M-AEN803 (2026W36-0009) with the #2397 repro
configuration (`aen-cc3501e-socket-throughput`, D-cache on, PL330 DMA,
`CONFIG_SPI_DW_ALIF_DMA_MIN_LEN=64`). Before the fix: `PING never answered`,
then the MPU fault. After: PING OK on the first attempt and the bridge sweep
running with no fault (CFSR `0x00000000`), 2 of 2 cold-boot runs.
