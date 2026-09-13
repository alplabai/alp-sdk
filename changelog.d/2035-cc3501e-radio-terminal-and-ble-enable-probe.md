### Fixed — a decoded CC3501E device-side failure was retried to the full poll budget instead of returned promptly, and the sentinel used to detect it was unsafe (#2035)

`chips/cc3501e/cc3501e_core.c`'s `resp_to_status()` maps three device-reported
codes onto `ALP_ERR_IO`, the same status it returns for a raw transport
hiccup: `ALP_CC3501E_RESP_ERR_RADIO` (`0x06`), `RESP_ERR_PROTOCOL` (`0x07`),
and `RESP_ERR_INTERNAL` (`0xFF`). `poll_by_repeat` could not tell "the device
answered a terminal failure" from "the wire glitched, retry" and retried all
three to the full poll budget. The firmware's per-seq retry latch (proto v8)
then answers every repeat from its latch without re-executing the operation,
so retrying a genuine decoded failure can never turn into success — it only
spends the whole budget (10 s for `BLE_ENABLE`'s `CC35_BLE_TIMEOUT_MS`) and
reports the wrong error class, `ALP_ERR_TIMEOUT`, instead of the real one,
`ALP_ERR_IO`.

An earlier version of this fix extended `poll_by_repeat`'s existing
`RESP_ERR_STATE` sentinel-peek at `ctx->rx_scratch[0]` to recognise only
`RESP_ERR_RADIO`, on the assumption that a pre-decode failure's `rx_scratch[0]`
residue could collide with `0x06` at worst 1-in-256. That assumption was
wrong: `0x06` is *also* `ALP_CC3501E_CMD_GET_CAPABILITIES`, a pre-decode
failure's `rx_scratch[0]` legitimately holds an echoed request opcode (the
in-band armed check and the `!hdr_ok` reject in `cc3501e_request_locked()`
both leave one there), and `GET_CAPABILITIES` is issued four calls before
`BLE_ENABLE` in the exact phase this fix targets — so the collision was
structurally likely on the path being fixed, not a rare fluke. Caught in
review before merge.

Fixed properly: `cc3501e_request_locked()` now writes a new marker,
`ALP_CC3501E_RX_SCRATCH_NO_STATUS` (`0xDA`, a documented-RESERVED bit-flip
neighbour of `ALP_CC3501E_RESP_OK` — never `0xFF`, which is itself a real
decoded `RESP_ERR_INTERNAL`), into `ctx->rx_scratch[0]` on *every* pre-decode
exit path, while the decoded path leaves the real status byte intact. The
sentinel now means "a status byte was actually decoded", closing the whole
residue surface instead of one opcode. `poll_by_repeat` peeks for all three
`ALP_ERR_IO`-mapped codes (`RESP_ERR_RADIO`, `RESP_ERR_PROTOCOL`,
`RESP_ERR_INTERNAL`) and returns promptly instead of retrying; every other
status is unchanged. The marker is public
(`ALP_CC3501E_RX_SCRATCH_NO_STATUS` in `<alp/chips/cc3501e/core.h>`, beside
`cc3501e_t::rx_scratch`) so code reading that field directly for diagnostics
— `aen-evk-demo`'s BLE_ENABLE probe below — can recognise it too.

**Contract reversal, not a stale test:** `tests/zephyr/cc3501e_ble_gatt_register`
previously pinned a decoded `RESP_ERR_RADIO` reply to retry to the full
budget and surface `ALP_ERR_TIMEOUT` (#480/#892). That contract is now
REVERSED for the reason above: `test_register_genuine_radio_fault_stays_unmasked`
now asserts a single round trip and `ALP_ERR_IO`. A new case,
`test_register_pre_decode_io_fault_still_retries`, was added — there was no
coverage at all before this change of a genuinely undecoded, pre-decode
`ALP_ERR_IO` (a raw transport hiccup, not a decoded device reply), which must
still retry to budget exactly as before.

### Documented — `aen-evk-demo` phase 8 now diagnoses a failed `BLE_ENABLE` instead of just failing it (#2035)

`BLE_ENABLE (0x30) -> -4` against `e1m-aen-evk-01` left two explanations
indistinguishable: the link wedged one transfer behind, or `BLE_ENABLE`
genuinely failed on the device with the failure masked as a timeout. Phase 8
now snapshots `rx_scratch[0..3]` left by `BLE_ENABLE`'s own last attempt
*before* issuing anything else (an earlier version of this probe incorrectly
claimed the post-`PING` read might still show that residue — false, since
`cc3501e_ping()`'s own phase-1 transceive unconditionally overwrites it),
issues a single-shot `cc3501e_ping()`, and classifies each `rx_scratch[0..3]`
snapshot into one of five shapes: the driver's own `ALP_CC3501E_RX_SCRATCH_NO_STATUS`
marker; parked idle marker (`0xA5` x4); the `#1378` dead-phase alias (`0x00`
x4, split out from an undriven line, `0xFF` x4, which used to share the same
"nothing is driving the line" string despite meaning two different things); a
**stale reply header** (opcode-shaped first byte, plausible declared length)
meaning the slave is answering one transfer behind — the pattern that
previously fell through to the generic, most-reassuring "structured data"
string for exactly the state that means the link is wedged; or, failing all
of those, genuinely unclassified structured data. `DIAG_GET_STATS`'s
`worker_execs`/`retry_latch_hits` are now printed only when
`has_worker_counters` is true (previously printed unconditionally with the
disqualifier tacked onto the same line). The three probe calls are now spaced
by `CC35_PING_GAP_MS` (320 ms, already used elsewhere in this file to clear
the CC3501E bridge firmware's 250 ms reply-stall watchdog) so the probe
itself does not suppress the link's only self-heal at the moment a wedged
link needs it.

Phase 8's opcode order is unchanged **only on the pass path**. On the
`BLE_ENABLE` failure path it now additionally emits `PING (0x00)`,
`DIAG_GET_STATS (0x70)`, and `GET_DIAG_INFO (0x04)` — three opcodes earlier
runs never emitted on a `BLE_ENABLE` failure.
