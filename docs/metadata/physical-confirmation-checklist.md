# Physical-layer confirmation checklist

Every chip below carries `physical:` data with `provenance: web_provisional` — transcribed from public datasheets by an automated sweep, **not yet maintainer- or bench-confirmed**. Confirm each against the authoritative datasheet (or the internal netlist for on-module parts), correct any pad, then flip `provenance` to `datasheet_confirmed`.

Priority: the AEN801 reference-carrier set (icm42670, tas2563, ina236, bmi323, bmp581, cam_mux_pi3wvr626, tcal9538) and the `⚠️ PAD MAP MISSING` rows (datasheet ball-map was not public — need internal/NDA source or a maintainer pin table).

Generated 2026-07-06 — 78 provisional parts.

| Chip | Package | Pins | Visibility | Note |
|------|---------|------|------------|------|
| `a02yyuw` | closed waterproof ultrasonic probe on captive cable, JST PH2.0-4P connector | 0 | public | module |
| `a4988` | QFN-28 | 26 | public |  |
| `act8760` | WLCSP-81 | 0 | public | module |
| `ar0234` | 83-ball CSP | 0 | public | module |
| `as5048a_b` | TSSOP-14 | 9 | public |  |
| `atecc608b` | UDFN-8 | 5 | public |  |
| `atgm336h` | 18-pad SMD module (castellated pads, 0.65/1.1mm pitch) | 0 | public | module |
| `bme280` | LGA-8 | 8 | public |  |
| `bmi323` | LGA-14 | 10 | public |  |
| `bmp390` | LGA-10 | 10 | public |  |
| `bmp581` | LGA-10 | 10 | public |  |
| `cam_mux_pi3wvr626` | QFN-24 | 24 | public |  |
| `cc3501e` | QFN (RSH), 56-pin | 0 | public | module |
| `clk_5l35023b` | VFQFPN-24 | 24 | public |  |
| `da9292` | WLCSP-54 | 54 | public |  |
| `deepx_dxm1` | M.2 2280, M-Key (board-down BGA variant used on V2N-M1; card variant shown) | 0 | public | module |
| `drv8825` | HTSSOP-28 | 27 | public |  |
| `drv8833` | HTSSOP-16 | 17 | public |  |
| `eeprom_24c128` | SOIC-8 | 8 | public |  |
| `es8388` | QFN-28 | 26 | public |  |
| `gc2145` | CSP-35 | 24 | public |  |
| `gd32_swd` | n/a -- debug-interface software module; no dedicated silicon (target: GD32G553MEY7TR, see gd32g553.yaml) | 0 | public | ⚠️ PAD MAP MISSING |
| `gd32g553` | WLCSP-81 | 18 | public |  |
| `gdew0154t8` | 24-pin FPC tail (bottom-contact ZIF), IL0373 driver IC | 0 | public | module |
| `hailo_8l` | M.2 Key B+M, 2242 (breakable to 2260/2280 per Hailo-8L M.2 ET spec) | 0 | public | module |
| `hx711` | SOP-16L | 16 | public |  |
| `icm42670` | LGA-14 | 10 | public |  |
| `ics_43434` | 6-pad bottom-port SMD | 6 | public |  |
| `il3820` | COG bare die (gold bump, waffle pack) | 24 | public |  |
| `ili9341` | COG bare die (bump-up) | 22 | public |  |
| `ili9488` | COG bare die | 17 | public |  |
| `imx219` | 65-pad CSP | 29 | public |  |
| `imx477` | 92-pin high-precision ceramic package (CLGA) | 27 | public |  |
| `ina236` | SOT-23-8 | 8 | public |  |
| `inmp441` | 9-terminal LGA (CAV) | 9 | public |  |
| `lps22hb` | HLGA-10L | 10 | public |  |
| `lsm6dso` | LGA-14L | 12 | public |  |
| `max31855` | SO-8 | 7 | public |  |
| `max31865` | TQFN-20 | 19 | public |  |
| `max98357a` | TQFN-16 | 12 | public |  |
| `maxim_max9295_9296` | ? | 0 | public | ⚠️ PAD MAP MISSING |
| `ms5611` | QFN-8 | 7 | public |  |
| `mt6701` | SOP-8 | 8 | public |  |
| `murata_lbee5hy2fy` | 72-pad LGA module (GND-shielded metal can) | 0 | public | module |
| `optiga_trust_m` | USON-10 | 5 | public |  |
| `ov2640` | 38-pin CSP2 | 32 | public |  |
| `ov5640` | 71-ball CSP BSI (13 NC) | 48 | public |  |
| `ov5645` | 66-pin CSP3 | 41 | public |  |
| `ov7670` | 24-ball CSP2 | 21 | public |  |
| `ov9281` | 64-pin CSP5 (4 NC) | 49 | public |  |
| `pca9451a` | HVQFN56 | 31 | public |  |
| `pi3dbs12212` | X2QFN-18 | 19 | public |  |
| `qmc5883l` | LGA-16 | 11 | public |  |
| `quectel_bg77` | SMD LGA module, 94 LGA pads | 0 | public | module |
| `quectel_bg95` | SMD LGA module, 102 LGA pads | 0 | public | module |
| `ra8875` | LQFP-100 | 11 | public |  |
| `rtl8211fdi` | QFN-40 | 35 | public |  |
| `rv3028c7` | SON-8 | 8 | public |  |
| `semtech_sx1262` | QFN-24 | 25 | public |  |
| `semtech_sx1276` | QFN-28 | 29 | public |  |
| `sh1106` | COG (Chip-On-Glass), no discrete board-mount package | 0 | public | ⚠️ PAD MAP MISSING |
| `ssd1306` | COG bare die (bump-up) | 38 | public |  |
| `st7789` | COG bare die (bump-up, Face Up) | 23 | public |  |
| `tas2563` | VQFN-32 | 20 | public |  |
| `tcal9538` | TSSOP-16 | 16 | public |  |
| `ti_ds90ub953_954` | VQFN-32 | 32 | public |  |
| `tlv320aic3204` | QFN-32 | 32 | public |  |
| `tmc2209` | QFN-28 | 28 | public |  |
| `tmp112` | SOT563-6 | 6 | public |  |
| `tps628640` | DSBGA-15 | 15 | public |  |
| `tsl2591` | ODFN-6 | 5 | public |  |
| `ublox_max_m10s` | 18-pad LCC (leadless chip carrier) | 0 | public | module |
| `ublox_neo_m9n` | 16-pad LCC (leadless chip carrier) | 0 | public | module |
| `ublox_sara_r5` | 96-pad LGA (miniature SARA form factor) | 0 | public | module |
| `veml7700` | OPLGA-4 (optical, side-view) | 4 | public |  |
| `vl53l1x` | Optical LGA12 | 11 | public |  |
| `vl53l5cx` | Optical LGA16 | 16 | public |  |
| `wm8960` | 32-lead QFN | 33 | public |  |
