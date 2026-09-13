### Fixed — the CC3501E PING retry loop starved the only recovery the link has (#2035)

`examples/aen/aen-evk-demo`'s first-`PING` retry used `CC35_PING_RETRIES 25` at
`CC35_PING_GAP_MS 200`. The bridge firmware self-heals a phase desync through a
reply-stall watchdog, `CC3501E_REPLY_STALL_MS = 250` in
`hal/ti/transport_hw_ti_spi.c`, and every host transceive re-stamps that
deadline. A 200 ms gap therefore re-armed the watchdog before it could ever
expire: a link that desynced once stayed desynced for all 25 attempts and
reported a part that was in fact answering.

That watchdog is the *only* recovery available. There is no chip-select to
resynchronise on, and byte-walking to realign provably parks the slave — see
the `cc3501e_sync()` warning in `chips/cc3501e/cc3501e_core.c`, which records
the reply header sticking at `0xA5A5A5A5` with the link never returning.

Measured on an E1M-AEN801 (M55-HE) against a wire-4.0 coprocessor image on
2026-09-10: `PING (0x00) -> -5 after 25 attempt(s) of 25`, with the host's
`rx_scratch` holding `04 00 08 00` — a structurally valid reply header for
`GET_DIAG_INFO` (`0x04`, declared payload length 8) — while the call that
actually failed was `cc3501e_diag_stats`, which sends `0x70`. A reply to the
*previous* request: the slave was exactly one transfer behind on MISO.

Both `aen-evk-demo` and `aen-cc3501e-handshake-probe` now retry 16 times at
320 ms. That clears the 250 ms watchdog with margin for the host's own
per-attempt transport time, and keeps the same ~5 s bound the original comment
justified. The relationship is now stated at both constants: a retry cadence
below `CC3501E_REPLY_STALL_MS` disables the link's self-heal, so the two
numbers may not be tuned independently.

This is the host half of the fix. The firmware half — the first-frame ISR
overshoot that caused the desync in the first place — lives in the
cc3501e-bridge-firmware repo.
