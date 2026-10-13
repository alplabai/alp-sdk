### Added — microSD + FatFs on the E1M-EVK for the E1M-AEN SoMs (#2856, #2857, #2858, #2860; Refs #2855)

- `zephyr/dts/alif/ensemble_e4_peripherals.dtsi`: `gpio2`, `i2c2`, `dma0`, `evtrtr0` and `sdhc0` for the E1M-AEN401 (Ensemble E4) M55-HP, values checked against the AE402 DFP `soc.h` (all disabled by default) (#2856).
- `e1m_evk_microsd` shield: enables `sdhc0` on the SD "B" pads (4-bit, 25 MHz, 3.3 V) with the `zephyr,sdmmc-disk` `"SD"` for E1M-AEN401/801/803. The app still drives the SDIO mux enable (E1M `IO20`, a CC3501E GPIO) low after the CC3501E bridge is up; only for carriers with the reworked 74LV3257 mux (#2857).
- `sdhc_dwc`: the ADMA engine cannot reach the M55 ITCM/DTCM, so `CONFIG_SDHC_DWC_BOUNCE_BUFFER` (default on for Ensemble) copies TCM buffers through an 8 KB per-instance buffer in SRAM0 and splits larger block transfers; the ADMA descriptor table now defaults to SRAM0 as well. DTCM-linked apps no longer need `zephyr,sram = &sram0` (#2858).
- `examples/aen/aen-sdcard-file`: bridge up, mux on, mount, write/sync/read back, list, free space, with `libraries: [fatfs]` (#2860).

Silicon, 2026-10-13, E1M-AEN803 2026W36-0002 on an EVK with the reworked mux, 128 GB exFAT card: the example passes on the M55-HE with the data cache on (4 KB and 48 KB files; a 96-sector multi-block read matches 96 single reads), and the M55-HP mounts and lists the card. The E4 nodes were bench-verified on 2026-10-12 from an app overlay with identical values; the bounce buffer is not yet run on an E4.
