### Fixed — PSA-crypto builds no longer overflow the main stack before main() (#2385)

With `ALP_SDK_MBEDTLS_PSA_CRYPTO` on (the default whenever the in-tree
mbedtls is built without TF-M), the PSA core seeds its DRBG during system
init, on the main thread's stack, before `main()` runs. That SHA-256 path
needs more than Zephyr's 1024-byte `CONFIG_MAIN_STACK_SIZE` default, so
apps that never raised it (`mqtt-telemetry`, `iot-fleet-ota`) hit
`Stack overflow on CPU 0` in `mbedtls_internal_sha256_process` before
printing anything. `zephyr/Kconfig.alp-libraries` now defaults
`MAIN_STACK_SIZE` to 4096 whenever the PSA core is on. An app or board
that sets its own size still wins.

Bench-verified on E1M-AEN803 2026W36-0009 (`e1m-aen-evk-01`, M55-HP,
`mqtt-telemetry` with the #2363 bridge overlay). The unfixed build faulted
at PC `0x00012410`. The fixed build (`CONFIG_MAIN_STACK_SIZE=4096` from the
new default) ran to `[mqtt] done`, and a `CONFIG_INIT_STACKS` run
measured a main-stack high-water mark of 1352 bytes by the end of CC3501E
bridge bring-up.
