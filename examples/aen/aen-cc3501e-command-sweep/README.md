# aen-cc3501e-command-sweep

Bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE). Exercises every
opcode the CC3501E Wi-Fi 6 / BLE 5.4 coprocessor's wire protocol defines and
measures inter-chip link throughput. Sibling of
`examples/aen/aen-cc3501e-handshake-probe` (read that app's README first if
you have not — it documents the bench this one shares, and the exact
`src/cc3501e_bridge.{c,h}` template this app reuses verbatim).

## What this app does NOT do

It never modifies `aen-evk-demo` or `aen-cc3501e-handshake-probe`. It never
flashes anything and never touches the bench beyond a RAM-run — build and run
it against a board already under your own bench session.

## Part 1: command coverage

`include/alp/protocol/cc3501e.h` defines 55 `ALP_CC3501E_CMD_*` opcodes
across nine families. `src/main.c` enumerates them by **symbol**, in
`g_all_cmd_opcodes[]` — never as a hand-copied hex literal — so the table
cannot silently drift from the header. Every one of the 55 is either:

- **invoked**, with its return code printed and classified as `OK` (the
  opcode succeeded), `REFUSED` (an *expected* refusal given this bench's
  actual hardware/credentials — e.g. `WIFI_GET_RSSI` returning
  `ALP_ERR_NOT_READY` because nothing is associated), or `FAIL` (anything
  else — a real transport/protocol fault worth investigating), or
- **skipped**, with a printed reason.

A self-check at the very end (`sweep_self_check()`) walks every opcode in
`g_all_cmd_opcodes[]` and fails loudly if any was neither invoked nor
skipped — the point being that "every opcode is accounted for" is a runtime
fact this app checks on itself, not a comment nobody re-verifies when a
future opcode gets added.

### Deliberately skipped, and why

| Opcode | Reason |
|---|---|
| `RESET` (0x02) | Reboots the bridge mid-sweep and would invalidate every result after it. |
| `OTA_BEGIN` / `OTA_WRITE` / `OTA_FINISH` / `OTA_PROMOTE` / `OTA_UPDATE_MODE` (0x40, 0x41, 0x42, 0x46, 0x47) | Write the coprocessor's flash and can leave it unbootable. `OTA_STATUS` (0x44) and `OTA_ABORT` (0x43) are read-only / session-reset-only and **are** invoked. |
| `WIFI_CONNECT_STA` (0x12) | Needs real AP credentials this app does not have and must not embed. |
| `WIFI_AP_START` (0x14) | Changes radio state (starts a soft-AP + DHCP server) and needs credentials this app does not have. |

Every other opcode is invoked. Two are worth calling out because they need a
peer this bench cannot promise exists, and are classified accordingly rather
than treated as failures:

- `CAM_ENABLE` (0x60): this bench has no camera fitted. `ALP_OK` here only
  means the LDO enable pin was driven — it does not by itself prove a camera
  is present; a refusal is the expected clean shape either way.
- `BLE_CONNECT` (0x36): targets an all-zero dummy peer address (bounded to a
  3 s timeout) purely to exercise the opcode — no real BLE peripheral is
  guaranteed present on this bench, so a timeout is the expected refusal, not
  a failure.

`SOCK_CONNECT` targets `192.0.2.1` (RFC 5737 TEST-NET-1, a
documentation-only address that is never routable) for the same reason: it
proves the opcode goes out on the wire without needing (or being able to
reach) a real destination.

### Ordering

Bring-up first, then META — both just to prove the link is alive. **Part 2
(throughput) then runs immediately**, before any of the remaining Part 1
families. Only after that does the rest of Part 1's coverage sweep continue,
in ascending opcode order (Wi-Fi → sockets → BLE → OTA → stream → GPIO →
SPI1 → camera/power → diagnostics, run **last** so its frame counters tally
the whole sweep — including Part 2's own traffic, since Part 2 now runs
before it, not just this one family's). Where a family has an
enable/disable pair, the disable half runs before the next family starts —
e.g. `BLE_GATT_REGISTER` runs **before** `BLE_ADV_START` (NimBLE refuses to
re-run `ble_gatts_start()` while advertising) and `BLE_DISABLE` is BLE's last
call, so no family inherits radio/session state a sibling left armed.

**Why throughput runs first:** coverage's own job is to deliberately provoke
refusals — `SOCK_CONNECT` at an unroutable test address, `BLE_CONNECT` at a
dummy peer, `WIFI_GET_RSSI` while unassociated — and each refusal spends a
real timeout proving what is already expected. On the bench run this app was
built from, one such timeout (`SOCK_CONNECT`, 3.05 s against a RFC 5737
address) left the link answering a mapped error to **every** opcode after
it, including `SOCK_CLOSE`, which had itself returned `ALP_OK` 20 ms earlier.
`aen-evk-demo` independently hit the identical shape behind a 10.6 s
`GET_MAC`. A long or timed-out operation, not a failing one, precedes this
link's wedge in both cases — so throughput (this app's headline number) runs
while the link is still known good, and coverage's own refusal-provoking
opcodes run after, where a wedge they trigger cannot cost Part 2 anything.

Like its sibling, this app **never stops early** on a failed step — a failed
step's own return code is the data it exists to collect. If bring-up itself
fails, every later opcode reports `ALP_ERR_NOT_READY` as a direct
consequence; STEP 1's own line is printed as the root cause so a reader does
not have to re-derive that from 55 repeated lines.

### Per-opcode timing

Every invoked opcode's line prints `elapsed_ms=<n>` alongside its `rc` —
`k_uptime_get()` deltas around the actual driver call, never
`k_cycle_get_32()` (this core runs at 160 MHz, and that counter wraps
roughly every 10.7 s at 400 MHz — the wrong clock for anything this sweep's
per-family runtime approaches). This is the datum that let the bench finding
above get made at all: without it, a 3 s `SOCK_CONNECT` and a 30 ms one print
an identical `REFUSED` line.

### Wedge detection

`sweep_report()` tracks consecutive **genuine `FAIL`** verdicts — never
`REFUSED`, since coverage provokes those on purpose and a refusal is not a
link symptom. At `SWEEP_WEDGE_FAIL_THRESHOLD` (3) FAILs in a row it prints
one `** SUSPECTED LINK WEDGE **` line naming the last opcode that returned
`OK` and the elapsed time of the operation that ran immediately before the
first failure in the streak — that operation, not the failures after it, is
the diagnostic signal per the bench finding above. This app does **not**
try to recover the link (no mid-sweep bridge reset — that would change what
is being measured, and the failure shape is worth capturing intact); it
keeps going and still accounts for every opcode.

**How to read a result after the trip:** every coverage line from that point
on is prefixed `[post-wedge]`. Read a `[post-wedge]` result as a
**consequence** of whatever operation the banner named, not as an
independent measurement of that opcode — a `[post-wedge] FAIL` on, say,
`GPIO_WRITE` says nothing about `GPIO_WRITE` itself; it says the link was
still wedged when `GPIO_WRITE` happened to run. The self-check at the end
(Part 3) still requires every opcode to be accounted for regardless, wedge
or not.

## Part 2: throughput

`STREAM_WRITE` (0x45) and `SPI1_TRANSFER` (0x56) are swept across 64, 256,
1024 bytes, plus each opcode's own **wire ceiling**:

- `STREAM_WRITE`'s ceiling is `cc3501e_stream_write()`'s own bound
  (`ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES`).
- `SPI1_TRANSFER`'s ceiling is `ALP_CC3501E_SPI1_MAX_XFER` minus the 2-byte
  CRC-16/CCITT-FALSE trailer headroom a wire-MAJOR-4 peer reserves (computed
  from `fw.fw_proto_major` at runtime — this coprocessor firmware negotiates
  protocol v4.0, so the CRC trailer applies), further clamped to whatever
  `SPI1_CONFIGURE`'s own reply reports as the peer's `max_xfer`.

Each size runs `SWEEP_THROUGHPUT_REPEATS` (5) times. **How to read the
table**: each line reports `bytes` (total bytes moved across the successful
repeats), `elapsed_ms` (the sum of `k_uptime_get()` deltas across those same
repeats), and `rate` derived from those two — each printed as a **separate**
figure so you can check the arithmetic by hand — plus the `min`/`max`
per-repeat elapsed time, so a single fast or slow outlier is visible rather
than averaged away. `k_uptime_get()` (not `k_cycle_get_32()`) is the clock:
this core runs at 160 MHz and `k_cycle_get_32()` wraps roughly every
10.7 s at 400 MHz, so it is the wrong tool for anything this sweep's total
runtime approaches.

`SPI1_TRANSFER`'s wire ceiling sample clocks real bits on the E1M
connector's SPI1 bus even though nothing is attached to it on this bench —
the CC3501E is the SPI **controller** on that path (it relays the host's
bytes to whatever, if anything, is downstream), so a `TRANSFER` completes
regardless of what is listening; `rx` is discarded (`NULL`) for every sample
since there is no real peripheral to read back from.

### What could not be measured, and why

`SOCK_SEND` / `SOCK_RECV` throughput is **not measured**. `WIFI_CONNECT_STA`
and `WIFI_AP_START` are deliberately never invoked (see above), so no Wi-Fi
association exists on this run and the firmware IP stack has no route to a
real peer — Part 1's SOCKETS coverage (which now runs AFTER this section —
see Ordering above) shows `SOCK_CONNECT` / `SOCK_SEND` / `SOCK_RECV` refused
for exactly that reason. A throughput
number over a connection that was never established would not be a
measurement of anything, so this app prints an explicit note instead of a
number.

## Board target

`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` — the same target
`aen-evk-demo` and `aen-cc3501e-handshake-probe` use, including their SRAM0
system-RAM placement and `CONFIG_DCACHE=n`. That placement is **load-bearing
for Part 2's numbers**, not decorative: the overlay's own header explains
that this app's Part 2 traffic rides the exact SPI1 link `aen-evk-demo`'s
phase 8 documents as sensitive to cache-on vs. cache-off timing on this
silicon (a DesignWare SSI master that underruns its TX FIFO deasserts its
own chip-select mid-frame) — running this app's sweep from a faster
cache-on/DTCM path than every other AEN801 bench app that has measured this
link would produce a throughput number nobody else's run is comparable to.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow):

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-command-sweep -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>;<fatfs>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-command-sweep/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

`<fatfs>` is only needed because `west list` in some worktree checkouts has
no `fatfs` entry even though the module is on disk — include it if your
build complains about a missing FatFs module; it is otherwise inert for this
app (it does not touch storage).

RAM-run over J-Link (no MRAM programming needed -- the overlay retargets
`zephyr,flash` to ITCM); read `ram_console_buf` over SWD, or the E1M edge
UART0 console if the bench has one wired (see `prj.conf`'s console toggle).
