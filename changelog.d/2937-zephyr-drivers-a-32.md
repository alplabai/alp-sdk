### Fixed — zephyr: PL330 chunking/stop/timeout, CDC200 DTCM scanout address, HE-only DMA2 latch (#2937, #2938, #2982, #2985)

dma_pl330_alif: in callback mode a block larger than one DMAGO could move
started every chunk back to back, rewriting the microcode of a running
channel and reporting the first completion as the whole block. Chunks are
now started one at a time: xfer() parks the remainder and the completion
ISR starts the next chunk. A polled timeout now kills the channel before
it is marked free (also for start_with_mcode). dma_stop no longer busy-waits
inside irq_lock(); the debug-window lock is held only around the idle check
and register writes. src_inc/dst_inc are set per block and DECREMENT is
rejected on every block, not just the head.

display_cdc200: the scanline ISR wrote the raw framebuffer pointer to
CFB_ADDR; it now translates through local_to_global() under
CONFIG_FB_USES_DTCM_REGION like cdc200_layer_fb_setup().

dma_alif_boot_latch: the M55HE_CFG (HE-local) DMA2 latch is built only
under CONFIG_RTSS_HE; header and CMake comment now match the code.
