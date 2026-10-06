### Changed (#2660)

- The E1M-V2N101 and E1M-V2M101 CM33 boards (`e1m_*_m33_sm`) carry a `bias-pull-up` on the sci0 pin group so RXD0 no longer floats (enabling sci0 later also needs `CONFIG_UART_INTERRUPT_DRIVEN=y`, documented in the defconfig), and advertise only the features whose nodes are enabled (`gpio`, `spi`; no `i2c`/`uart`). The sci0 console stays disabled (audit CM33-01, IO-01, CM33-M3, CM33-14).
- The same boards add a 16 KiB `CONFIG_RAM_CONSOLE` buffer (`zephyr,ram-console`) at CM33-NS `0x9f710000` (A55 `0x4f710000`), inside the reserved OpenAMP window and clear of the rsctbl page and liveness beacon (`0x9f700ff0..0x9f700fff`), the mhu-shm page and the vrings; the A55 can read it post-mortem (audit CM33-M2). Not yet built or run on hardware.

### Fixed

- `spi_renesas_rz_sci_b.c`: the PM9 comments now use the real encoding (`0b01` is INPUT, `0b11` is OUTPUT with input enabled, there is no push-pull mode), the stale P94/SD1_CD clobber reference is gone (card detect is `PA1`), and `PWPR.REGWE` is left set rather than restored from a possibly stale read (removes one stale-restore hazard; a Linux pinctrl clear of REGWE between the CM33 set and its PMC9/PM9 writes can still drop them, so the PWPR race is not closed and this is not bench-verified). `zephyr/CMakeLists.txt` notes that hal_renesas compiles `r_dmac_b` under `CONFIG_USE_RZ_FSP_DMAC_B`; the DMA path stays gated (audit CM33-03, CM33-04, CM33-10).
