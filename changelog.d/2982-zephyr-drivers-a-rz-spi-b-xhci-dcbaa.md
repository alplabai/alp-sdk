### Fixed — zephyr: RZ SPI-B CS left asserted on empty buffers and unbounded polled waits, xHCI DCBAA overrun (#2982)

spi_renesas_rz_spi_b: transceive() asserted CS before checking for an
all-empty buffer set and then returned without deasserting it; CS is now
asserted only once there is data to move. The polled SPTEF, SPRF and IDLNF
waits are bounded (100 ms) and fail the transfer with -ETIMEDOUT, still
deasserting CS and disabling the engine, instead of hanging the caller on a
clock-gated or wedged RSPI.

uhc_xhci_alif: CONFIG.MaxSlotsEn is clamped to the 8 slots dcbaa[9] has
entries for, and an Enable Slot ID beyond the array is reported as no slot,
so dcbaa[slot] can no longer be written out of bounds.

The other four items of #2982 (CDC200 scanout address, DMA latch scope,
PL330 stop under irq_lock, PL330 src_inc/dst_inc) were fixed in #3032.
