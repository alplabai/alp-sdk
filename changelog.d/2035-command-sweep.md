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

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement and `CONFIG_DCACHE=n` as its sibling
`aen-cc3501e-handshake-probe`, for the same reason: the SPI1 FIFO-refill
timing this link is sensitive to). See
`examples/aen/aen-cc3501e-command-sweep/README.md`.
