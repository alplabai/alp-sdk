### Fixed — seven CC3501E test suites' software SPI models now frame a real MAJOR-4 reply (#2035)

`tests/zephyr/cc3501e_transport_lock`, `cc3501e_host_ota`, `cc3501e_host_events`,
`cc3501e_host_driver`, `cc3501e_ble_gatt_register`, `cc3501e_console_wifi` and
`cc3501e_wifi_backend_security` each carry a hermetic software model of the
CC3501E firmware SPI slave. When the wire protocol moved to MAJOR 4 (`ALP_CC3501E_RESP_OK`
off `0x00` onto `0x5A`, plus a CRC-16/CCITT-FALSE reply trailer), every model's
bare `RESP_OK` reply was updated to the new status byte but never grew the
trailer -- so `cc3501e_reply_verdict()` (`chips/cc3501e/cc3501e_core.c`), which
infers MAJOR-4 shape from a `0x5A` status byte even before `fw_proto_major` is
negotiated, correctly rejected every one of them. `host_driver` alone carried
26 pass / 64 fail.

Added `tests/zephyr/cc3501e_common/cc3501e_reply_model.h`, a shared header
(`alp/protocol/crc16.h`-backed) all seven suites now include: `cc3501e_model_stage_reply()`
builds the real MAJOR-4 shape (status + data, zero-padded to an
`ALP_CC3501E_REPLY_PAD` multiple, CRC trailer in the last two bytes), and
`cc3501e_model_stage_legacy_reply()` builds the plain MAJOR-3 shape for the
handful of cases that must stay unpadded (an `ALP_CC3501E_RESP_ERR_*` status,
or a wire shape -- a genuinely short reply, an unpadded attribute value -- a
real MAJOR-4 firmware could never produce). Each suite's `stage_status()`/
`stage_reply()` now dispatches on the status byte to the matching helper
instead of hand-rolling the frame.

Two independent, pre-existing test-model bugs surfaced once host_driver's
reply frames were real enough for the driver to actually decode them:
`g_connect_submit_force_ok` (the WIFI_CONNECT_STA / WIFI_AP_START dead-phase-alias
mutant) was staging the new `ALP_CC3501E_RESP_OK` (`0x5A`) instead of
`ALP_CC3501E_RESP_OK_LEGACY` (`0x00`, the literal byte an all-zero dead SPI
phase actually clocks back); and `test_reset_accepts_lower_minor_0033`'s
`MINOR - 1` override value was composed at `uint16_t` width, so the intended
byte-wise wraparound corrupted the MAJOR byte it was OR'd with.

`host_events` also drops its own bespoke `pad_replies`/`model_apply_reply_padding()`
in favour of the shared helper, now applied unconditionally (a real firmware
pads every reply, not just the ones a test opts into).

All seven suites are green: `transport_lock` 1/1, `host_ota` 13/13,
`host_events` 14/14, `host_driver` 91/91, `ble_gatt_register` 4/4,
`console_wifi` 5/5, `wifi_backend_security` 4/4.

Added one explicit legacy (MAJOR-3) regression,
`test_ping_accepts_legacy_major3_bare_ok_shape_bilingual` in `host_driver`:
pokes `ctx->fw_proto_major` to `ALP_CC3501E_PROTOCOL_MAJOR_LEGACY` and drives
a real `cc3501e_ping()` against an `ALP_CC3501E_RESP_OK_LEGACY` (`0x00`,
unpadded, no CRC) reply -- the actual frame a 3.1 firmware sends -- proving
the "host is bilingual" migration-window contract
(<alp/protocol/cc3501e.h>'s migration-order note) end to end through a real
driver call, not just inferred from the two model-bug fixes above. Left the
other six suites without new legacy cases: none of them negotiate
`fw_proto_major` themselves (that gate lives entirely in `cc3501e_reset()`,
which only `host_driver`'s suite exercises), so a legacy case anywhere else
would need the same ad hoc `fw_proto_major` poke with no corresponding
opcode-specific behaviour left to distinguish -- not cheap, and not adding
coverage `host_driver`'s case doesn't already provide.
