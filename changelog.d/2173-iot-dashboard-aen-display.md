### Fixed — `iot-dashboard` builds for AEN now, with a real panel (#2173)

Item (3) of #2173. `iot-dashboard`'s AEN scenario named `ensemble_e8_dk/...`
— Alif's own DevKit board, which no `-p` flag in this repo selects — so it
built nowhere, and #2173's mbedTLS fix (already on `dev` via #2193) could not
retarget it the way it retargeted `mqtt-telemetry` and `iot-fleet-ota`: LVGL
needs a `zephyr,display` chosen node, and this app's declared ST7789 has no
working SPI route on the E1M EVK (its SPI0 pads are all repurposed on the
2626-R2 netlist). The maintainer's call: give the demo the RK055HDMIPI4MA0
DSI panel #2204 wires up for `aen-dsi-display`, rather than inventing a new
panel bring-up.

`CMakeLists.txt` now appends the `e1m_evk_rk055hdmipi4ma0` shield before
`find_package(Zephyr)`, but only for this app's own AEN M55-HP board targets
(`examples/connectivity/iot-dashboard/CMakeLists.txt:60`
("list(APPEND SHIELD e1m_evk_rk055hdmipi4ma0)")) — a regex match on `BOARD`,
resolved from a `-DBOARD=` cache entry or `$ENV{BOARD}` for a plain build, or
read out of the sysbuild cache file for a `--sysbuild` build (the AEN flow's
documented default — see `docs/_aen-runbook-section.md` and
`scripts/bench/aen/README.md`), because sysbuild does not forward `BOARD` as
a `-D` cache entry to the image build at all; Zephyr only resolves it inside
`find_package(Zephyr)`, too late for this regex. Skipped
when the caller already set `SHIELD`, so a different panel can still be
substituted, and a no-op on `native_sim` and every other target. `board.yaml`
drops the `st7789` chip entry it no longer needs — the panel isn't declared
there at all, since it has no `metadata/chips/*.yaml` manifest of its own and
arrives as a Zephyr shield instead
(`examples/connectivity/iot-dashboard/board.yaml:61`
("The DSI panel (RK055HDMIPI4MA0, Himax HX8394) has no")) — and every stale
ST7789/SPI-display comment in `prj.conf`, `native_sim.conf`, the
`native_sim` board overlay, and `src/main.c`'s bring-up diagram is corrected
to describe the DSI chain instead. The `spi` and `gpio` peripheral entries
`board.yaml` still declares are unchanged in substance: both were already the
CC3501E bridge's (SPI transport, WIFI_EN/nRESET control), not the display's.

`testcase.yaml`'s AEN scenario now names both AEN SKUs the bench farm
carries, matching how `pr-twister-aen.yml` shards by SKU
(`examples/connectivity/iot-dashboard/testcase.yaml:40-41`
("alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp")), `build_only: true`
throughout, and carries its own `CONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y`
(#2192 — no Ensemble entropy driver exists yet, so the weak-RNG refusal in
`zephyr/CMakeLists.txt` applies here the same way it does to
`mqtt-telemetry`) on the scenario rather than in `prj.conf`, so a copy
scaffolded by `tan init --from-example` for real hardware still meets the
refusal. `pr-twister-aen.yml` adds `examples/connectivity/iot-dashboard` to
both `paths:` filter blocks, adds `zephyr/boards/shields/**`, `zephyr/drivers/display/**` and
`zephyr/drivers/mipi_dsi/**` (the shield and the CDC200 / DesignWare DSI
drivers it compiles into this app, none previously covered by this gate), and
adds the
example as a sixth bounded `--testsuite-root`
(`.github/workflows/pr-twister-aen.yml:450`
("alp-sdk/examples/connectivity/iot-dashboard")) — the one root among the six
that contributes a scenario to *both* SKU matrix legs, since mqtt-telemetry
and iot-fleet-ota remain AEN801-only.

Verified locally (Zephyr v4.4.1 with `zephyr/patches.yml` applied, Zephyr SDK
1.0.1 `arm-zephyr-eabi-gcc` 14.3.0, picolibc; plain twister for both SKU legs
plus one `--sysbuild` configure-and-link): `iot-dashboard` links for both
`alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp` and
`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp` with the shield applied and
`CONFIG_ALP_SDK_CHIP_ST7789` gone from the generated config; `mqtt-telemetry`
and `iot-fleet-ota` still link for AEN801 unchanged.

**Review found this shipped no AEN board overlay at all (#2242).**
`board.yaml` declares `spi` + `gpio` for the CC3501E bridge
(`cc3501e_bridge_bringup()`, `src/cc3501e_bridge.c`, bus_id
`CC3501E_BRIDGE_SPI_BUS_ID` = 1, `alp_pins` WIFI_EN/NRST), but the shield
added above wires only the display chain — it does nothing for SPI1 pinmux
or the LP-GPIO control nets. Without a board overlay, `alp_spi_open(1)` and
the `alp_pins` array had no DT node to resolve on either AEN target: the
app compiled, but could never reach the CC3501E, on real hardware or in
CI's `build_only` scenario (the DTS just lacked the node — nothing caught
it because `check_example_board_overlay_parity.py` checks for a `boards/`
overlay only where one is declared, and none was). Two new overlays fix
this — the CC3501E bridge subset of `alp-console`'s bench-validated
wiring, no I2C2/RGB-LED (this app has no sensor-manifest bus of its own
here):
`examples/connectivity/iot-dashboard/boards/alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay:68`
("cc3501e_spi: spi@48104000 {") and
`examples/connectivity/iot-dashboard/boards/alp_e1m_aen803_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay:108`
("alp_pins: alp-pins {"). This also turns on real coverage from
`check_example_board_overlay_parity.py` (#2101) and
`check_example_board_overlay_content_parity.py` (#1009/#2198) for this
example's AEN801/AEN803 pair, neither of which had anything to check
before these files existed. Verified: both HP targets link with
`west build`, and `zephyr.dts` in each build tree shows `spi@48104000`
`status = "okay"` with `pinctrl-0 = <&pinctrl_spi1>` and the `alp-spi1`
alias resolved, sourced from the new overlay rather than left undeclared.

**Not proven, and not silently left implicit.** This is a `build_only`
compile check, nothing more:

- No silicon has run this image. There is no pixel-on-glass confirmation for
  this app's use of the RK055HDMIPI4MA0/HX8394 chain, and the display chain
  itself depends on #2204 (the shield), which as of this change is a draft PR
  whose own bench evidence is an M55-HE run, not M55-HP.
- The new CC3501E bridge overlays are HP twins of the bench-proven M55-HE
  wiring (`alp-console`, `aen-cc3501e-bringup`) — the SPI1 IRQ and the
  whole HP bridge path are unbenched, same BENCH-TBD caveat the M55-HE
  overlays already carry.
- The build still only compiles because of the #2192 weak-RNG acknowledgement.
  That flag is bench-only; a production build on real hardware needs a real
  entropy source, which does not exist for AEN yet.
- The dashboard's own UI layout is unchanged at 240×320 and is not adapted to
  the RK055's 720×1280 panel — it draws in the panel's top-left corner.
