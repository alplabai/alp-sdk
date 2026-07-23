# aen-cc3501e-ble-gatt

Bench proof of the **#480 CC3501E BLE GATT-SERVER path** through the
portable [`<alp/ble.h>`](../../../include/alp/ble.h) surface, on the
**E1M-AEN801** (Alif Ensemble E8) SoM's M55-HE core.

**Server-only — no live central peer on this bench.** There is no second
BLE device in range, so the app never calls `alp_ble_connect()`. See
`src/main.c`'s top-of-file comment for the exact PASS contract this shapes:
`alp_ble_open()` and `alp_ble_advertise_start()` are genuine over-the-bridge
wire round-trips to the CC3501E's own NimBLE host; `alp_ble_gatt_register_service()`
is expected to return `ALP_ERR_NOSUPPORT` (the portable per-characteristic
GATT encoder for this backend is a later slice —
[`src/backends/ble/cc3501e.c`](../../../src/backends/ble/cc3501e.c)); and
`gatt_read`/`gatt_write`/`gatt_notify` are exercised with a `NULL` connection
handle to prove the dispatch layer's connection guard rejects them with the
documented status instead of a crash or a hang. A full read/write echo
against a live central peer is HIL-deferred — it needs a second BLE device
on the bench.

## SoM bring-up is one call

Same reusable `cc3501e_bridge_bringup()` template as
[`aen-cc3501e-bringup`](../aen-cc3501e-bringup) /
[`aen-cc3501e-companion-tour`](../aen-cc3501e-companion-tour) — see
`src/cc3501e_bridge.h`.

## Console: RAM console, not UART

`board.yaml` sets `diagnostics.console: ram` — on the alplab bench, UART5
isn't captured (only the SE-UART FT232R is on USB, SETOOLS-only), so the
`RESULT PASS:`/`RESULT FAIL:` line is read from `ram_console_buf` over SWD
(see `flashing-and-bench-debugging-aen`), not a serial terminal.

## Builds verified (off-silicon)

- **native_sim/native/64** (`twister`, `build_only`) — against the emulated
  SPI controller + `alp,pin-array` in `boards/native_sim_native_64.overlay`
  (same emulation shape as `aen-cc3501e-companion-tour`; it proves the
  portable-API call shapes compile, not a live GATT round-trip — the CC3501E
  SPI bridge has no native_sim model).
- **alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he** — the real bench target;
  build + Flow-C/A/D flash is a bench step (`scripts/bench/aen/*`), not a CI
  run.
