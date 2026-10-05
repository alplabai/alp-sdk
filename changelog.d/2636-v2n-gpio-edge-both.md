### Fixed — SD card detect is polled instead of requesting an unsupported both-edge interrupt (#2636)

The RZ/V2N ICU has no both-edge TINT mode, so the MMC core's request for the card-detect GPIO interrupt failed on every boot (`genirq: Setting trigger mode 3 ... failed`) and the core fell back to polling. `&sdhi1` now sets `broken-cd`: no interrupt is requested, the core polls the `cd-gpios` line about once a second, and insertion and removal are still detected within about a second, as before. Built into the dtb; not run on a board.
