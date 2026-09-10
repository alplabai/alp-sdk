### Fixed — three MAJOR-4 opcodes lost dead-phase protection, a `cc3501e_reset()` restore was missing, and five wrapper maxima went stale (#2035)

A review of all 47 command opcodes against `<alp/protocol/cc3501e.h>`'s reply
definitions found three wrong, all three losing protection on the wire the
classification exists to serve.

**`BLE_GATT_REGISTER` (`0x38`) had no dead-phase guard at all.** Its reply is
`in_status(1) | num_handles(1) | attr_handle[num_handles](LE16)` — never
legitimately all-zero, since `num_handles == num_chars` is at least 1 on
success — but the opcode was missing from `cc3501e_reply_carries_data()`
(`chips/cc3501e/cc3501e_core.c`). On a MAJOR-3 peer, a payload phase that died
clocked back eight `0x00` bytes and `cc3501e_ble_gatt_register()` reported
`ALP_OK` with zero handles: a service the caller believed registered, on a
dead link. Added to `cc3501e_reply_carries_data()`, deliberately NOT to
`cc3501e_reply_may_be_all_zero()`.

**`WIFI_SCAN_START` (`0x10`) and `BLE_SCAN_START` (`0x34`) were missing too,
and a doc comment claimed the opposite.** Both opcodes carry the packed
record list the host walks, so a dead phase during a scan read back as
`ALP_OK` with a count of zero — "no networks found" on a dead bus. Unlike
`BLE_GATT_REGISTER`, an empty scan IS a legitimate result indistinguishable
by shape alone from the dead-phase alias, so both opcodes are now in BOTH
`cc3501e_reply_carries_data()` and `cc3501e_reply_may_be_all_zero()`, with a
comment explaining the trade. The stale doc comment listing
"`WIFI_SCAN_START`/`STOP`" among bare-status opcodes is corrected to
`WIFI_SCAN_STOP`, `BLE_SCAN_STOP` (the opcodes that actually stayed
bare-status).

**`cc3501e_reset()` zeroed the peer dialect but never restored it on a
transport hiccup.** The zeroing at the top of `cc3501e_reset()` is correct —
it stops a stale major from mis-framing the `GET_VERSION` request that
follows. But the `vs != ALP_OK` branch a few lines down returned `ALP_OK`
with a comment claiming the context was "left as it was", while
`fw_proto_major`/`fw_proto_minor` stayed zeroed. A re-reset on a healthy
MAJOR-4 peer whose `GET_VERSION` round trip merely misses — the comment two
lines up calls this the common case immediately after a reset — left
`fw_proto_major == 0` with `initialised == true`: `want_req_crc()` then
framed every later request CRC-less, and a MAJOR-4 firmware rejected each one
with `RESP_ERR_INVALID` until some later reset happened to get a version
through. The prior major/minor are now saved before zeroing and restored on
that branch.

**Five wrapper maxima went stale against the request ceiling.** The prior
follow-up (see the sibling `#2035` fragment) tightened `cc3501e_request()`'s
own `tx_len` ceiling by `ALP_CC3501E_CRC_BYTES` once a MAJOR-4 peer is
negotiated, but `cc3501e_ble_gatt_register()`, `cc3501e_ble_gatt_notify()`,
`cc3501e_ble_gatt_write()` (`chips/cc3501e/cc3501e_ble.c`) and
`cc3501e_sock_send()` (`chips/cc3501e/cc3501e_sockets.c`) still accepted the
bare, un-tightened maximum — rejecting the request two frames deeper with no
explanation a caller sized to the documented constant could read. All four
now subtract `ALP_CC3501E_CRC_BYTES` too, and the `@param len` docs on
`cc3501e_spi1_transfer()` (`include/alp/chips/cc3501e/core.h`) and
`cc3501e_ota_write()` (`include/alp/chips/cc3501e/ota.h`) are corrected to
state the same tighter bound `cc3501e_request()` actually enforces.

**Test coverage gap that let the first two findings ship.** No test staged a
dead phase on the legacy MAJOR-3 wire for an opcode that carries data.
`tests/zephyr/cc3501e_host_driver/src/test_host_driver.c` gains three tests
staging an all-zero, `ALP_CC3501E_REPLY_PAD`-padded legacy reply for
`BLE_GATT_REGISTER`, `WIFI_SCAN_START`, and `BLE_SCAN_START` directly at the
`cc3501e_request()` transport layer — the first must reject with
`ALP_ERR_IO`, the two scans must accept as a legitimate empty result.
Verified by mutation: removing each opcode from its matching classification
list reddens the corresponding test; restored after confirming.

**Two nits.** A comment in `cc3501e_request()` named a
`cc3501e_ble_write_descriptor()` that does not exist in the tree; corrected
to name the real wrapper functions and the ceiling they must stay in
lockstep with. `tests/zephyr/cc3501e_common/cc3501e_reply_model.h` claims to
be the one place that knows a real reply's wire shape, but always pads,
where the firmware pads only when the pad still fits the frame buffer;
unreachable at the sizes the seven suites this header serves stage today, so
the header's claim is narrowed rather than the edge case reproduced.

Also fixes one test fixture: `test_spi1_configure_encodes_request_and_decodes_reply`
staged `ALP_CC3501E_SPI1_MAX_XFER` (4088, the pre-CRC-subtraction constant)
as the `SPI1_CONFIGURE` reply's `max_xfer`, when a real MAJOR-4 firmware
reports 4086 — the rest of the suite models MAJOR-4 replies throughout, so
this one field was the inconsistency.

`west twister -p native_sim/native/64` across all seven CC3501E suites
(`host_driver`, `host_ota`, `host_events`, `transport_lock`,
`ble_gatt_register`, `console_wifi`, `wifi_backend_security`): 135/135 test
cases pass.
