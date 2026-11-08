### Fixed — spi_dw_alif: ISR error path keyed on the transfer's DMA use, not the node's DMA config (#2940)

spi_dw_isr() took its DMA error branch whenever the node had DMA configured,
so an RX overflow or TX overrun on an interrupt-driven PIO transfer (short,
async or slave) on a DMA-capable node gave a semaphore nobody waits on and
never called completed(): a slave waited forever and an async callback never
fired. The ISR now keys on a per-transfer flag set from the same use_dma
decision transceive() makes, so PIO transfers complete with -EIO.
