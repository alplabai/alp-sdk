### Fixed — mqtt-telemetry and iot-fleet-ota can reach the CC3501E on their M55-HP target (#2363)

Both examples target only the M55-HP (`alp_e1m_aen801_m55_hp`) but shipped
no board overlay, so the HP had no SPI1 node, `alp-spi1` alias or
`alp_pins` WIFI_EN/NRST, and `cc3501e_bridge_bringup()` returned
`ALP_ERR_NOT_PRESENT_ON_THIS_SOC`. Each now ships the same M55-HP bridge
overlays `iot-dashboard` got in #2242, for AEN801 and AEN803, and both
examples' AEN scenario now also builds for
`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`.

First M55-HP bridge evidence on silicon: on E1M-AEN803 2026W36-0009
(`e1m-aen-evk-01`), `mqtt-telemetry` RAM-run on the M55-HP (with the #2385
main-stack fix) printed `[mqtt] wifi: cc3501e_bridge_bringup -> ALP_OK`.
It ran through `alp_wifi_connect()` (`ALP_ERR_TIMEOUT` against the
placeholder SSID, as expected) to `[mqtt] done`. `iot-fleet-ota` was built,
not run.
