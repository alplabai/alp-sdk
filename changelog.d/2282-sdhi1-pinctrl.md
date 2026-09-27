### Fixed — carrier microSD (SDHI1) now works on the E1M-X EVK (#2282)

`e1m-x-evk.dtsi` left `sdhi1_pins` empty, so `pinctrl-rzg2l` logged
`no mapping found in node /soc/pinctrl@10410000/sd1` and SDHI1
(`mmc@15c10000`) never probed. The SD1 node is now complete:

- `sdhi1_pins` carries the dedicated `SD1CLK`, `SD1CMD` and `SD1DAT0-3` pads.
- Card detect is a real `cd-gpios` on `PA1` (`SD1_SD1CD`), active-low.
- `vqmmc_sdhi1` (now in `e1m-v2n-som.dtsi`, where the selector lives) switches
  the SD1 IO rail on `PA2` (`µSD1_V_SEL`): low = 3.3 V, high = 1.8 V.
- UHS is enabled up to SDR104 (200 MHz). The SD1 pads use output-impedance
  2, one step weaker than the eMMC: at 3 (strongest) SDR104 through the carrier
  SDIO mux fails the data phase with CRC errors (`-84`).
- Card power (`PA3`, `SD1_SD1PWEN`) stays an always-on hog: the card-detect
  pull-up is on the switched card rail, so cutting power would make an empty
  slot read as occupied. `SDCARD_RST` (`PA4`) is the M.2 Wi-Fi SDIO reset and
  stays undriven.

Bench (E1M-V2M103 on the E1M-X EVK, blank GD32, 32 GB SDHC card): `mmc1: new
ultra high speed SDR104 SDHC card`, 512 MiB read in 6.5 s (~78 MB/s), two
1.5 GiB reads with identical md5, 10/10 unbind/bind re-enumerations at SDR104,
no errors.

U-Boot still numbers `mmc 1` as SDHI2 (`mmc@15c20000`, the Wi-Fi SDIO
controller); its device tree needs the same SD1 node before `mmc` there
reaches this slot.
