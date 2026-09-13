### Fixed — the MAJOR-4 dead-phase guard, request-size ceiling, and a stale wire dialect after `cc3501e_reset()` (#2035)

**The all-zero dead-phase guard rejected every ordinary legacy (MAJOR 3)
bare-`RESP_OK` reply.** The firmware zero-pads a reply's payload up to an
`ALP_CC3501E_REPLY_PAD` (8-byte) multiple with the pad folded INTO the
declared payload length (see `chips/cc3501e/cc3501e_events.c` and
`ALP_CC3501E_REPLY_PAD`'s doc comment), so a real bare-status reply's wire
`payload_len` is never `1` — it is always the padded length, typically `8`.
`cc3501e_reply_verdict()` (`chips/cc3501e/cc3501e_core.c`) dispatched its
two dead-phase mechanisms on `payload_len == 1u`, a shape no real bridge
ever produces, so every bare-status opcode (`PING`,
`GET_PENDING_EVENTS`, `GPIO_CONFIGURE`/`WRITE`, `SOCK_CONNECT`/`BIND`/
`LISTEN`/`CLOSE`, `WIFI_DISCONNECT`, `OTA_BEGIN`/`WRITE`/`PROMOTE`, most
`BLE_*`, ...) fell through to the multi-byte "has real data" branch and was
wrongly rejected as a dead phase on a legacy peer, stranding every board
still on 3.1 firmware — the OTA path off 3.1 needs a working `PING`/event
poll to run at all. Dispatch is now on a new `cc3501e_reply_carries_data()`
classification of the OPCODE, not the padded wire length — the fact that
distinguishes the two mechanisms was never in `payload_len` to begin with.

**`cc3501e_reset()` left a stale peer dialect latched.** It re-armed
`ctx->initialised` but not `ctx->fw_proto_major`/`fw_proto_minor`, so a
retry against a different-dialect peer (or a peer whose first `GET_VERSION`
reply merely garbled) framed that very `GET_VERSION` request in the
PREVIOUS peer's dialect — a stale major-4 latch appends a CRC trailer a 3.1
peer rejects outright, `GET_VERSION` never completes, and the
transport-hiccup path returns `ALP_OK` with the bad major still latched: a
permanently wedged link reporting success. Both fields are now cleared
alongside `initialised` before the version read.

**The request-size ceiling wasn't tightened for the CRC trailer.**
`cc3501e_request()` / `poll_by_repeat()` still accepted `tx_len ==
ALP_CC3501E_MAX_PAYLOAD` even against a MAJOR-4 peer, where the request
build appends a 2-byte CRC trailer — encoding a wire length past the
protocol's own maximum into the header's length field. The ceiling is now
`ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_CRC_BYTES` whenever the negotiated
peer wants the trailer.

**A hard preprocessor error broke every GD32G553 Zephyr build.**
`src/zephyr/gd32g553_ota_crc_zephyr.c` used `DT_HAS_COMPAT_STATUS_OKAY()`
before including `<zephyr/devicetree.h>` — the file's only other includes
are deliberately Zephyr-free — which is a hard `missing binary operator
before token "("` on every build, `&&` short-circuiting notwithstanding
(macro expansion runs over the whole preprocessor line before the
expression is evaluated). `tests/zephyr/chips` was among the builds this
broke. Fixed by including the header.

**Docs and public-header comments corrected to match the code:**
`docs/cc3501e-bridge.md` said the current wire is 3.1 (it is 4.0), that
`0x00` is `ALP_CC3501E_RESP_OK` (it is `0x5A` from MAJOR 4; `0x00` is the
legacy-only `RESP_OK_LEGACY`), and that the host refuses a mismatched
version outright (the gate is bilingual across MAJOR 3 and 4, on purpose,
for the OTA migration path). `include/alp/protocol/cc3501e.h`'s headline
sentence for MAJOR 4 ("adds a CRC trailer to every frame, both directions")
contradicted the code's own `GET_VERSION`-is-CRC-optional carve-out, stated
only in a later parenthetical — now a prominent normative rule next to the
headline, matching what the firmware side implements and tests.
`include/alp/chips/cc3501e/core.h`'s `fw_proto_major` doc comment said a
mismatch refuses the link; 3 is a usable value now, and the comment
previously told callers not to branch on it when they must.

**Test:** `tests/zephyr/chips/src/test_cc3501e.c` gains
`test_cc3501e_reply_verdict_major3_bare_status_is_padded`, fabricating the
real 8-byte padded shape a bridge actually sends (the existing
`..._major3_round_trip` test's `payload_len == 1` fixture is a shape no
real bridge produces, which is why this shipped green). Verified by
mutation against a standalone gcc harness: `ALP_ERR_IO` before the fix,
`ALP_OK` after, with sibling GET_MAC / WIFI_CONNECT_STA all-zero cases
still correctly rejected either way.
