### Added — `aen-cc3501e-connect-twice-probe` tests whether a station-never-associated `WIFI_DISCONNECT` clears the CC3501E vendor SDK's disconnect-in-progress bit (#2035)

Reading the TI vendor SDK source
(`source/ti/net/wifi_stack/app_entry/wlan_if.c` and `.../cme/cme.c`,
`simplelink_wifi_sdk_10_10_01_08`) turned up a chain that may be the root
cause of every failed association in this campaign: `Wlan_Disconnect()`
sets `WLAN_IF_DISCONNECT_IN_PROGRESS` in the SDK's `g_oper_bitmap`, but on
the station branch — the path a plain STA disconnect always takes — it
returns without clearing that bit; only the access-point branch and the
`fail:` label clear it inline, and the only other clear is the
`WLAN_EVENT_DISCONNECT` handler. `Wlan_Connect()` gates on the same bitmap
via an allow-mask that does not include the disconnect bit, so while it is
set, every connect returns `RET_OPER_IN_PROGRESS` immediately, which the
bridge firmware maps to `ALP_CC3501E_WIFI_FAIL_KICK`.

The one unproven link is whether the supplicant emits
`WLAN_EVENT_DISCONNECT` when asked to disconnect a station that never
associated. This new bench app tests exactly that, without touching
firmware state directly: `cc3501e_wifi_connect()` already issues a
`WIFI_DISCONNECT` at entry whenever `WIFI_STATUS` reads `CONN_FAILED` from a
previous attempt (`#1435`) — that entry clear is itself the call that would
set the bit if attempt one never associated, so letting
`cc3501e_wifi_connect()`'s own entry-clear logic run twice back to back with
the same credentials exercises the real path. The app scans first —
load-bearing, not decoration: on the scan-first ordering the link survives
a failed connect (measured, 2 of 2), while connect-first wedges it and
nothing afterwards can be read — then runs two full-budget (55 s) connect
attempts and prints each one's terminal `WIFI_STATUS` `state` and
`fail_reason` as raw numbers and decoded names. A closing verdict reads the
pair: `FAIL_TIMEOUT` then `FAIL_KICK` confirms the bit sticks; identical
reasons on both attempts means it does not; a successful attempt one means
the run says nothing about the hypothesis. Any other pair is printed with
no verdict claimed beyond it.

Uses its own `CONNTWICE_WIFI_SSID` / `_PASS` / `_SECURITY` build-time
defines (empty SSID default — the app says so and stops before scanning or
connecting) so a combined build cannot collide with any sibling
`aen-cc3501e-*` app's own credential macros. Never issues `OTA_*`, `RESET`,
or a soft-AP.

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement, and `CONFIG_DCACHE=n` as
`aen-cc3501e-command-sweep`, for the same SPI1 link-timing reason). See
`examples/aen/aen-cc3501e-connect-twice-probe/README.md`.
