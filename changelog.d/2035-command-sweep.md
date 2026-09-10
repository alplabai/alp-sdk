### Added — `aen-cc3501e-command-sweep` exercises the CC3501E's full command surface and measures link throughput (#2035)

Every `ALP_CC3501E_CMD_*` opcode `<alp/protocol/cc3501e.h>` defines (55 across
nine families — meta, Wi-Fi, sockets, BLE, OTA, stream, GPIO proxy, SPI1 host
passthrough, camera/power, diagnostics) is now either invoked with its return
code classified (success / expected refusal given this bench's actual
hardware and credentials / genuine failure) or explicitly skipped with a
printed reason — never silently absent. A runtime self-check at the end of
the run walks every opcode symbol and fails loudly if any was neither
invoked nor skipped, so a future opcode this app forgets to add shows up as
a gap instead of passing quietly.

`OTA_BEGIN`/`OTA_WRITE`/`OTA_FINISH`/`OTA_PROMOTE`/`OTA_UPDATE_MODE` and
`RESET` are never invoked (flash-write / bridge-reboot risk); `OTA_STATUS`
and `OTA_ABORT` are read-only and are. `WIFI_CONNECT_STA` and
`WIFI_AP_START` are never invoked (no credentials, no SSID/passphrase
embedded anywhere in the app).

Part 2 measures `STREAM_WRITE` and `SPI1_TRANSFER` throughput across a
64/256/1024-byte sweep plus each opcode's own wire ceiling (the
`SPI1_TRANSFER` ceiling is computed with the wire-MAJOR-4 CRC-trailer
headroom this bench's coprocessor firmware negotiates), 5 repeats per size
reporting bytes / elapsed ms / derived rate and the min–max spread, not a
single sample. `SOCK_SEND`/`SOCK_RECV` throughput is explicitly not
measured — no Wi-Fi association exists on this run, so there is no real
peer to measure a rate against.

Part 2's throughput sweep now runs immediately after bring-up and the META
family, **before** the rest of Part 1's opcode coverage, not after it. A
first bench run put coverage first and measured nothing: coverage
deliberately provokes refusals (`SOCK_CONNECT` at an unroutable test
address, `BLE_CONNECT` at a dummy peer), each refusal burns a timeout, and
one such timeout (`SOCK_CONNECT`, 3.05 s) left the link answering every
later opcode with a mapped error, including opcodes that had themselves
succeeded moments earlier — so Part 2 ran against an already-wedged link and
derived no rate at any size. Reordering means throughput measures against a
link proven live by bring-up + META, before coverage's own refusals can put
it at risk; coverage still accounts for all 55 opcodes exactly once
afterward, and `sweep_self_check()`'s `opcodes_missing` count is unaffected
by which half runs first.

Every invoked opcode's line now also prints `elapsed_ms=<n>`
(`k_uptime_get()` deltas, not `k_cycle_get_32()` — this core is 160 MHz and
that counter wraps roughly every 10.7 s at 400 MHz), the datum the first
bench run's diagnosis had to reconstruct from capture timestamps outside
the app. `sweep_report()` also now tracks consecutive genuine `FAIL`
verdicts (never `REFUSED` — coverage provokes those on purpose) and, at 3 in
a row, prints one `** SUSPECTED LINK WEDGE **` line naming the last opcode
that succeeded and the elapsed time of the operation immediately before the
first failure in the streak — the long/refused operation, not the failures
that follow it, is the diagnostic signal the first bench run's trace
pointed at. Every result after the trip is tagged `[post-wedge]` so it
reads as a consequence, not an independent measurement; this app never
resets the bridge mid-sweep to try to recover, since that would change what
is being measured. `SOCK_CONNECT`'s own timeout is also cut from 2000 ms to
100 ms — long enough to still exercise the opcode against a healthy link,
short enough that the refusal it deliberately provokes no longer costs the
rest of the sweep a multi-second wedge risk.

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement and `CONFIG_DCACHE=n` as its sibling
`aen-cc3501e-handshake-probe`, for the same reason: the SPI1 FIFO-refill
timing this link is sensitive to). See
`examples/aen/aen-cc3501e-command-sweep/README.md`.
