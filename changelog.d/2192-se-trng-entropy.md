### Added — AEN boards seed their CSPRNG from the Secure-Enclave TRNG by default (#2192)

AEN had no Zephyr entropy driver, so the mbedTLS PSA core fell back to
`TEST_RANDOM_GENERATOR`, and #2193's refusal made every TLS build opt in
to that weak RNG explicitly. A new `alif,se-trng` entropy driver
(`zephyr/drivers/entropy/entropy_alif_se.c`, `CONFIG_ENTROPY_ALIF_SE`)
fixes this. It is thin glue over hal_alif's public
`se_service_get_rnd_num()`, chunked to the SE's 256-byte per-request
limit, with no ISR variant. `ensemble_e8_peripherals.dtsi` carries a
disabled `se_trng` node next to `se_service`.

`ensemble_e8_peripherals.dtsi` enables the SE-service channel
(`se_service`, `seservice0r/s`) and `se_trng`, and chooses
`zephyr,entropy = &se_trng`. Every AEN board therefore gets
`CONFIG_CSPRNG_ENABLED=y` on real hardware entropy. The weak-RNG opt-ins
are gone from the `mqtt-telemetry`, `iot-fleet-ota` and `aen-se-crypto`
AEN scenarios; the native_sim scenarios keep theirs. `aen-se-crypto`
gained a `zephyr,entropy` step: two 300-byte draws, which cross the chunk
limit, plus `sys_csrand_get()`.

**Caveat (#1700):** the first CSPRNG draw is now an SE `GET_RND` request.
On a module with a mismatched SERAM/services pair, that is the reported
trigger for the M55-HP dropping to 76.8 MHz. ADR 0030 makes such a pairing
unsupported, and a matched v110 pair was re-tested clean: the CGU
oscillator and PLL registers were unchanged across the request. An app
that must avoid SE traffic can disable `&se_trng` and choose its own
`zephyr,entropy`.

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

A board-default build of `aen-se-crypto` passed again, `aen-gpio-bench`
(no entropy consumer) still passes with the SE channel linked, and
`mqtt-telemetry` on the M55-HP boots without the test-entropy warning.
