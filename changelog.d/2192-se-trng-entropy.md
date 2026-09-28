### Added — Secure-Enclave TRNG entropy driver for Alif Ensemble (#2192)

AEN had no Zephyr entropy driver, so the mbedTLS PSA core fell back to
`TEST_RANDOM_GENERATOR`, and #2193's refusal made every TLS build opt in
to that weak RNG explicitly. A new `alif,se-trng` entropy driver
(`zephyr/drivers/entropy/entropy_alif_se.c`, `CONFIG_ENTROPY_ALIF_SE`)
fixes this. It is thin glue over hal_alif's public
`se_service_get_rnd_num()`, chunked to the SE's 256-byte per-request
limit, with no ISR variant. `ensemble_e8_peripherals.dtsi` carries a
disabled `se_trng` node next to `se_service`.

A build opts in by enabling `&se_service`, `&seservice0r`, `&seservice0s`
and `&se_trng`, and choosing `zephyr,entropy = &se_trng`. That gives
`CONFIG_CSPRNG_ENABLED=y` with no weak-RNG opt-in needed.
`aen-se-crypto`'s M55-HE overlays now do this, and the example gained a
`zephyr,entropy` step: two 300-byte draws, which cross the chunk limit,
plus `sys_csrand_get()`.

Bench-verified on E1M-AEN803 2026W36-0009 (M55-HE, Flow C RAM-run), 2 of
2 `RESULT PASS`:
- `zephyr,entropy (se_trng): get=0/0 differ=1 csrand=0 OK`;
- no `Using a test - not safe - entropy source` warning at boot;
- the CSPRNG output differed across boots (`dfcf56a3…` vs `36884390…`),
  which a test generator would repeat.

The same build for the M55-HP
(`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`), using the same
`seservice0r/s` pair, also passed:
`zephyr,entropy (se_trng): get=0/0 differ=1 csrand=0 OK`. The
`0x40040000`/`0x40050000` mailboxes are core-local, so each core reaches
the SE through its own copy.

**Not done, so #2192 stays open.** The AEN boards do not choose it by
default yet. The connectivity examples (`mqtt-telemetry`,
`iot-fleet-ota`, `iot-dashboard`) keep their weak-RNG opt-ins.
