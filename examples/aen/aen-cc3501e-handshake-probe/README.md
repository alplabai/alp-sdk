# aen-cc3501e-handshake-probe

Diagnostic bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE).
Discriminates competing explanations for a CC3501E Wi-Fi 6 / BLE 5.4
coprocessor link failure seen on real silicon today, after the coprocessor
was reflashed to a wire-protocol-4.0 image.

## The failure

After the CC3501E coprocessor was reflashed to a wire-protocol-4.0 image
that requires a CRC-16/CCITT-FALSE trailer on every frame, phase 8 of
`examples/aen/aen-evk-demo` produced:

```
CC3501E: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ 25000000 Hz) -> 0
CC3501E: PING (0x00) -> -5 after 25 attempt(s) of 25 (200 ms apart)
```

`-5` is `ALP_ERR_IO`. It does not discriminate: `resp_to_status()` in
`chips/cc3501e/cc3501e_core.c` maps `ALP_CC3501E_RESP_ERR_PROTOCOL` (`0x07`)
-- a *correctly received* error reply -- onto the *same* `ALP_ERR_IO` a dead
link produces. The demo cannot tell them apart on its own: it never prints
`fw_proto_major`, and it gates `GET_VERSION` behind a successful `PING`, so
a `PING` failure short-circuits the phase before `GET_VERSION` ever runs.

This app does **not** modify `aen-evk-demo`.

## This app's own bug, and why the table below looks the way it does

A first version of this app was itself unsound (bench run + review,
2026-09-10). It printed `HYPOTHESIS C (image not booting/answering)` about a
part that was **demonstrably answering**. The only reason anyone found out
was a human reading `fw.rx_scratch` over SWD by hand after the run: it held
`04 00 08 00` -- a structurally valid `GET_DIAG_INFO` reply header (echoed
opcode `0x04`, declared payload length `8`). A dead, undriven link cannot
produce that; it reads as all `0x00` or all `0xFF`. That version's decision
table called hypothesis C whenever step 3 failed, full stop -- ignoring the
bring-up result, the major, and (because it printed no wire bytes at all)
any evidence that the part had actually answered something.

Every change below traces back to that one failure, and the point of all of
them together is the same: **this app must never again print a confident
verdict the wire itself disproves, and it must never again need a human
with a debugger to see what the wire actually said.**

## The states this app can report

| State | Meaning |
|---|---|
| `BRIDGE_ABSENT` | Step 1 could not open the bridge's pins/SPI controller at all. Not a link fault -- there is nothing to talk to. |
| `VERSION_SKEW` | The firmware **answered** a `GET_VERSION` (link demonstrably alive) with a major this host refuses -- major `0` means pre-versioning-scheme firmware (a raw v1..v9 integer); anything else outside `{3, 4}` is a genuine, unsupported skew. |
| `HYPOTHESIS_C` | Bring-up succeeded, `fw_proto_major` never left `0`, **and** every wire dump collected this run looked undriven (all `0x00` or all `0xFF`). This is now the *only* combination this app will call "not booting/answering". |
| `LINK_ALIVE_UNPARSED` | The wire showed unmistakable signs of life (a parked-idle `0xA5` marker, or a structured candidate header) but no step completed cleanly enough to support a named hypothesis. **This is what the original bug's own six numbers now map to** -- see the walkthrough below. |
| `HYPOTHESIS_A` | `cc3501e_reset()`'s own `GET_VERSION` missed (major was still `0` after step 1); this app's own direct `GET_VERSION` got major `4`; the now-CRC'd `PING` succeeded. |
| `HYPOTHESIS_A_PLUS_B` | Same precondition as A, but the CRC-trailer `PING` *still* fails once major `4` is latched. |
| `HYPOTHESIS_B` | Major `4` was *already* latched by step 1 (reset's own `GET_VERSION` worked), yet the CRC-trailer `PING` fails. |
| `LEGACY_3_1_ACTIVE` | Firmware answers with major `3` (the pre-4.0 wire) and `PING` succeeds against it -- link fully healthy, just still running the OLD image (the OTA to 4.0 has not landed). Neither A nor B applies: major 3's `PING` has no CRC-trailer payload phase at all. |
| `HANDSHAKE_HEALTHY` | Major `4` latched and the CRC-trailer `PING` against it succeeds. Neither A nor B reproduces. |

Major `3` used to be silently folded into "major `0` means dead" by the old
table (it only ever checked "major `== 4`?"), and a firmware still on 3.1
firmware -- meaning the reflash to 4.0 did not take -- had no state of its
own to land in. See `handshake_classify()` in `src/main.c` for the exact
gates and reasoning behind every row above.

## How this app tells them apart

### Steps

1. **Bring the bridge up** exactly the way `aen-evk-demo` phase 8 does
   (`cc3501e_bridge_bringup()`, byte-identical to that app's own
   `src/cc3501e_bridge.c`/`.h` -- WIFI_EN high, nRESET pulsed, SPI1 @
   25 MHz). Also runs `cc3501e_reset()`'s own internal `GET_VERSION`
   attempt.
2. **Print `fw_proto_major`/`fw_proto_minor`** immediately, read directly
   off the public `cc3501e_t` struct fields -- `0` means reset's own
   `GET_VERSION` missed (or the bridge never came up).
3. **Call `cc3501e_get_version()` directly.** `cc3501e_get_version()` is a
   *bare round trip* -- it writes only `*version_out`, **never**
   `ctx->fw_proto_major` (that field is written only inside
   `cc3501e_reset()`) -- so this call goes out under whatever major step 1
   left, exactly like any other caller. A failure here gets its wire bytes
   dumped (see "Reading a wire dump" below).
4. **Latch the major -- explicitly, and say so.** `cc3501e_get_version()`
   does not touch `ctx->fw_proto_major` (see step 3), so without this
   assignment step 5's `PING` would go out under whatever step 1 left --
   identical framing to the demo's own already-failed `PING`, testing
   nothing new. This app writes `fw.fw_proto_major`/`fw.fw_proto_minor`
   directly from step 3's own decoded reply -- the same public-field-write
   access pattern step 2 already uses to *read* them -- and prints
   `SYNTHETIC latch` on the console line so a reader cannot mistake this
   for a negotiation `cc3501e_reset()` actually performed.
5. **`PING`, bounded and retried** -- `HANDSHAKE_PING_RETRIES` /
   `HANDSHAKE_PING_GAP_MS` (see `src/main.c` for the current figures and
   why the gap has to clear the bridge firmware's reply-stall watchdog). A
   single attempt cannot tell "the shape is broken" from "the slave had
   not finished booting yet during this one attempt". Framed against
   whatever step 4 just latched; the console line names the dialect.
6. **`GET_DIAG_INFO` and `DIAG_GET_STATS`.** `last_error` lives in
   `GET_DIAG_INFO`'s reply; the frame counters (`frames_ok`/`frames_err`)
   live in the *separate* `DIAG_GET_STATS` reply -- the driver exposes
   them on two different opcodes. **Both** now print `(no reply)` instead
   of the struct's zero-initialiser when their own call fails -- printing
   `frames_ok=0 frames_err=0` as if measured, when the struct was in fact
   never written, was exactly the shape of the original bug repeated one
   call later.
7. **Print a one-line verdict** from the explicit decision table (see
   `handshake_classify()` in `src/main.c`).

Then a `RESULT:` line summarising every number collected, including whether
any wire dump this run showed life.

### Reading a wire dump

After **any** step that fails, this app dumps `fw.rx_scratch[0..7]` and
`fw.tx_scratch[0..7]` as hex, decodes the first 4 rx bytes as a candidate
reply header (`frame[2] | frame[3] << 8` for the declared length -- the same
rule `decode_header_payload_len()` in `chips/cc3501e/cc3501e_core.c` uses),
and classifies the shape into one of three cases:

- **`UNDRIVEN` (all `0x00` or all `0xFF`)** -- nothing is driving the bus.
  Consistent with an absent, dead, or unpowered slave.
- **`PARKED IDLE` (all `0xA5` = `ALP_CC3501E_SYNC_IDLE`)** -- the slave IS
  armed and driving the bus, just parked at a frame boundary (the in-band
  armed check `cc3501e_request_locked()` uses). Alive, whatever this call's
  own return code says.
- **`STRUCTURED` (anything else)** -- real, non-uniform data. The slave
  answered *something*. This is the case the original bug's own DTCM read
  found by hand; this app now prints it on the console instead.

Only `UNDRIVEN` counts as "the wire looks dead"; both other shapes flip
`link_shows_life` to true, which is what keeps hypothesis C from firing.

## Walkthrough: the original bug's exact numbers, through the new table

The transcript that shipped the wrong verdict was:

```
STEP 1: bridge bring-up (...) -> 0
STEP 2: fw_proto_major=0 fw_proto_minor=0
STEP 3: cc3501e_get_version() -> -5  raw=0x0000  decoded=v0.0
STEP 4: fw_proto_major=0
STEP 5: cc3501e_ping() -> -5
STEP 6a: cc3501e_diag_info() -> -5  last_error=0x00 (no reply)
STEP 6b: cc3501e_diag_stats() -> -5  frames_ok=0 frames_err=0
```

...plus the fact only discovered afterwards, by hand, over SWD:
`fw.rx_scratch` held `04 00 08 00` -- a structured header matching step 6a's
own opcode (`ALP_CC3501E_CMD_GET_DIAG_INFO = 0x04`).

Feeding that same input into `handshake_classify()`:

- `probed = true` (step 1 returned `ALP_OK`).
- `step2_major = 0`.
- `step3_ok = false` (step 3 returned `-5`).
- With this app's wire-dump instrumentation now in place, **step 6a's own
  console line would have printed that `04 00 08 00` dump itself**,
  classified `STRUCTURED` -- so `link_shows_life = true` for this run,
  established before step 7 ever runs.
- `handshake_classify()`'s first gate: bring-up succeeded, so we reach
  `if (!step3_ok) { if (step2_major == 0 && !link_shows_life) return
  HYPOTHESIS_C; return LINK_ALIVE_UNPARSED; }`. `step2_major == 0` is true,
  but `link_shows_life` is **also** true, so the `HYPOTHESIS_C` branch does
  **not** fire.
- Result: **`LINK_ALIVE_UNPARSED`** -- "the wire showed unmistakable signs
  of life ... but no step completed cleanly enough to support A, B, or a
  healthy verdict. This is NOT hypothesis C: whatever is wrong here, the
  part is not dead."

That is the sound answer for this input set: the table no longer prints a
confident "dead" verdict the wire itself already disproved. What it
*cannot* do from this data alone is pick a more specific cause -- the
`04 00 08 00` header was structurally clean, so whatever went wrong,
happened in a later phase of that specific exchange (the payload
transceive, most likely, given the header phase alone completed) -- that
is a real open question this app is not built to answer by itself, and
`LINK_ALIVE_UNPARSED` says exactly that instead of guessing.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow), same target
as `aen-evk-demo`:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-handshake-probe -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-handshake-probe/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

RAM-run over J-Link (no MRAM programming needed -- the overlay retargets
`zephyr,flash` to ITCM); read `ram_console_buf` over SWD, or the E1M edge
UART0 console if the bench has one wired (see `prj.conf`'s console toggle).

The overlay also retargets `zephyr,sram` to global SRAM0 (matching
`aen-evk-demo`'s own phase-8 memory placement) and `prj.conf` sets
`CONFIG_DCACHE=n` -- see the overlay's own header for why: phase 8's SPI1
FIFO-refill loop is the one phase the demo's own comment calls out as
timing-sensitive on this silicon (a DW-SSI master that underruns its TX
FIFO deasserts its own chip-select mid-frame), and a probe built to explain
that demo's own failure cannot measure a *faster* link on precisely that
axis and call it a controlled comparison.

This app does **not** flash anything and does not touch the bench beyond a
RAM-run -- build and run it against a board already under your own bench
session.
