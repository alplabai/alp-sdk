### Added — `qenc-readout` builds and runs on the E1M EVK with an AEN SoM (#2095)

`examples/peripheral-io/qenc-readout` gains board overlays for the
AEN801/AEN803 M55-HE and M55-HP targets
(`examples/peripheral-io/qenc-readout/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`,
and the AEN803 twin) that bind `alp-qenc0` to a `gpio-qdec` node on the
EVK rotary encoder's phase pads, so the portable `alp_qenc_*` calls reach
the encoder through the gpio-qdec backend added in #2506. A new
build-only twister scenario, `alp_sdk.example.qenc_readout.aen`, keeps the
two targets building. Bench, E1M-AEN803 serial 2026W36-0001, M55-HE RAM-run:
`alp_qenc_open` and `alp_qenc_reset_position` return `ALP_OK` and the
position reads `0` at rest; a turn of the knob has not been captured yet.
