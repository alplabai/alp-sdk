### Added — `aen-cc3501e-handshake-probe` discriminates a CC3501E PING `-5` without ever needing a human on SWD (#2035)

After the CC3501E coprocessor was reflashed to a wire-protocol-4.0 image that
requires a CRC-16/CCITT-FALSE trailer on every frame, phase 8 of
`examples/aen/aen-evk-demo` produced `bridge bring-up -> 0` followed by
`PING (0x00) -> -5 after 25 attempt(s)`. `-5` (`ALP_ERR_IO`) does not
discriminate: `resp_to_status()` in `chips/cc3501e/cc3501e_core.c` maps
`ALP_CC3501E_RESP_ERR_PROTOCOL` (`0x07`) — a correctly received error reply —
onto the same code a dead link produces, and the demo never prints
`fw_proto_major` or reaches `GET_VERSION` once `PING` has already failed.

This new bench app does not touch `aen-evk-demo` and instead runs the
identical bring-up, then discriminates `BRIDGE_ABSENT` / `VERSION_SKEW`
(firmware answered `GET_VERSION` with a major outside `{3, 4}` — including
`3` itself, which the first cut of this table silently folded into "major 0
means dead") / `HYPOTHESIS_C` (image not answering) / `LINK_ALIVE_UNPARSED`
(the wire showed life but no step parsed cleanly) / `HYPOTHESIS_A` /
`HYPOTHESIS_A_PLUS_B` / `HYPOTHESIS_B` / `LEGACY_3_1_ACTIVE` (still on the
pre-4.0 image, fully healthy) / `HANDSHAKE_HEALTHY`. A bench run plus review
on 2026-09-10 found the first cut of this app itself unsound: it printed
`HYPOTHESIS C (image not booting)` about a part that had, in fact, echoed a
structurally valid `GET_DIAG_INFO` reply header (`04 00 08 00`) — visible
only because a human read `fw.rx_scratch` over SWD by hand after the run,
since the app printed no wire bytes at all. Every fix below closes exactly
that gap.

`cc3501e_get_version()` is a bare round trip that never writes
`ctx->fw_proto_major` (only `cc3501e_reset()` does), so this app now **latches
it explicitly** — writing the public `fw_proto_major`/`fw_proto_minor` fields
directly from its own step 3 reply right before `PING` — and prints
`SYNTHETIC latch` on the console so that write is never mistaken for a
negotiation the driver performed. After any failing step, it now dumps
`rx_scratch`/`tx_scratch[0..7]` as hex and classifies the candidate reply
header as `UNDRIVEN` (all `0x00`/`0xFF` — genuinely dead), `PARKED IDLE` (all
`0xA5` — armed and alive), or `STRUCTURED` (real data) — this is what would
have shown the `04 00 08 00` header in the console transcript itself.
`handshake_classify()`'s decision table now gates `HYPOTHESIS_C` on the wire
actually looking undriven (not just on a failed call), keys the major on
`{0, 3, 4, other}` instead of only checking for `4`, feeds the version
reply's own decoded major into the branch that matters, and adds
`LINK_ALIVE_UNPARSED` for exactly the case the original bug produced. `PING`
is now a bounded, retried loop (matching `aen-evk-demo`'s own figures) rather
than one attempt, and `DIAG_GET_STATS`' `(no reply)` marker now matches
`GET_DIAG_INFO`'s, so a `-5` never prints its zero-initialiser as a
measurement. The app's overlay/`prj.conf` also now match `aen-evk-demo`'s
phase-8 memory placement (global SRAM0, `CONFIG_DCACHE=n`) instead of running
faster than the demo on the one axis its own comment calls out as
timing-sensitive (a DW-SSI TX-FIFO underrun deasserting its own chip-select
mid-frame), and `CONFIG_ALP_SDK_CONSOLE=n` keeps the Zephyr shell — dragged
in transitively by `CONFIG_ALP_SDK`'s own default — out of the transcript.

Never stops early on a failed step — a failed step's own return code is the
data this app exists to collect. Builds for
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, the same target
`aen-evk-demo` uses; RAM-runs over J-Link like its sibling AEN bench apps.
