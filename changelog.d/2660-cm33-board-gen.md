### Changed (#2660)

- The E1M-V2N101 and E1M-V2M101 CM33 boards (`e1m_*_m33_sm`) carry a `bias-pull-up` on the sci0 pin group so RXD0 no longer floats, set `CONFIG_UART_INTERRUPT_DRIVEN=y` so the SCI rxi/eri IRQs are connected if sci0 is ever enabled, and advertise only the features whose nodes are enabled (`gpio`, `spi`; no `i2c`/`uart`). The sci0 console stays disabled (audit CM33-01, IO-01, CM33-M3, CM33-14).
- The same boards add a 16 KiB `CONFIG_RAM_CONSOLE` buffer (`zephyr,ram-console`) at CM33-NS `0x9f710000` (A55 `0x4f710000`), inside the reserved OpenAMP window and clear of the rsctbl page and liveness beacon (`0x9f700ff0..0x9f700fff`), the mhu-shm page and the vrings; the A55 can read it post-mortem (audit CM33-M2). Not yet built or run on hardware.

### Fixed

- `spi_renesas_rz_sci_b.c`: the PM9 comments now use the real encoding (`0b01` is INPUT, `0b11` is OUTPUT with input enabled, there is no push-pull mode), the stale P94/SD1_CD clobber reference is gone (card detect is `PA1`), and `PWPR.REGWE` is left set instead of restoring a possibly stale value that could re-lock a concurrent Linux pinctrl write. `zephyr/CMakeLists.txt` notes that hal_renesas compiles `r_dmac_b` under `CONFIG_USE_RZ_FSP_DMAC_B`; the DMA path stays gated (audit CM33-03, CM33-04, CM33-10).
- The generated V2N101/V2M101 CM33 board `.dts` no longer claims no WDT0 register base exists in-tree: hal_renesas defines `R_WDT0_BASE 0x41C00400` (`wdt_iodefine.h`). There is still no `wdt0` node because no driver binds the SoC's WDT yet.
