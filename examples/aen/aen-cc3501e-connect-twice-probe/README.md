# aen-cc3501e-connect-twice-probe

Bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE). Tests one specific
hypothesis about the CC3501E Wi-Fi 6 / BLE 5.4 coprocessor's vendor SDK:
whether a `WIFI_DISCONNECT` issued against a station that never associated
clears the SDK's `WLAN_IF_DISCONNECT_IN_PROGRESS` bit. Sibling of
`examples/aen/aen-cc3501e-command-sweep` (same bring-up template and board
overlay — read that app's README first if you have not).

## The chain this app tests (#2035)

Read directly from the TI vendor SDK sources
(`source/ti/net/wifi_stack/app_entry/wlan_if.c` and `.../cme/cme.c`,
`simplelink_wifi_sdk_10_10_01_08`) — read them yourself before trusting this
summary:

1. `Wlan_Disconnect()` calls `set_cond_in_process_wlan_discconnect(1)`, which
   sets `WLAN_IF_DISCONNECT_IN_PROGRESS` in the SDK's own `g_oper_bitmap`.
2. On the **station branch** (the `default:` case of `Wlan_Disconnect()`'s
   `switch`, which is what a plain STA disconnect always takes) it **returns
   without clearing that bit** — only the access-point branch and the
   `fail:` label clear it inline.
3. The only other clear is the `WLAN_EVENT_DISCONNECT` handler.
4. `Wlan_Connect()` gates on the same bitmap via
   `set_cond_in_process_wlan_connect()` → `is_wlan_oper_in_progress()`, whose
   allow-mask for connect is
   `ROLE_UP_AP | ROLE_DOWN_AP | SET | GET | GET_EXCLUDE | SET_EXCLUDE`. The
   disconnect bit is **not** in it.
5. So while that bit is set, every `Wlan_Connect` returns
   `RET_OPER_IN_PROGRESS` immediately, which the bridge firmware maps to
   `ALP_CC3501E_WIFI_FAIL_KICK`.

**The one unproven link, and the only thing this app exists to test:**
whether the supplicant emits `WLAN_EVENT_DISCONNECT` when asked to
disconnect a station that never associated. If it does, the bit clears and
none of this bites. If it does not, the bit sticks for the rest of the
session and every later connect dies at the kick.

## How this app exercises that path without touching firmware state

`cc3501e_wifi_connect()` (`chips/cc3501e/cc3501e_wifi.c`) already issues a
`WIFI_DISCONNECT` at **entry** whenever the `WIFI_STATUS` latch reads
`CONN_FAILED` from a previous attempt (see that function's own `#1435`
comment). That entry clear *is* the call that sets the bit in the first
place if attempt one never associated — attempt two exercising it is
exactly the real path this app cares about. This app never bypasses it and
never calls `cc3501e_wifi_disconnect()` directly around it; it lets
`cc3501e_wifi_connect()`'s own, already-shipped, entry-clear logic run
twice back to back with the same credentials.

## Why scan first

On the scan-first ordering the link survives a failed connect — measured,
2 of 2. Connect-first wedges it and nothing afterwards can be read. Without
the scan this app cannot report anything at all. STEP 3 runs
`cc3501e_wifi_scan()` and prints the record count before either connect
attempt for exactly that reason.

## What this app prints, and what it means

Both connect attempts run to a **terminal** `WIFI_STATUS` (a full 55 s
budget, not a shortened one — unlike `aen-cc3501e-wedge-postmortem`, this
app wants a real terminal outcome, not a mid-wedge snapshot). Each attempt's
`state` and `fail_reason` are printed as raw numbers *and* decoded names.
STEP 6 then prints the pair and one of:

- **CONFIRMED** — attempt one reports `FAIL_TIMEOUT`, attempt two reports
  `FAIL_KICK`: the bit stuck across the two attempts.
- **DEAD** — both attempts report the *same* reason: the bit does not stick.
- **says nothing** — attempt one *succeeded*: there was no failure to leave
  the bit set behind, so this run cannot speak to the hypothesis.
- Any other pair (attempt two also succeeds, either attempt does not reach
  a terminal state, or the reasons differ but not in the predicted way) is
  printed as-is with no verdict claimed beyond it — see `src/main.c`'s STEP
  6 for the exact cases. This app prints the pair and what the pair means;
  it does not editorialise beyond that.

## Security value on the wire — do not mix the two encodings up

`cc3501e_wifi_connect()`'s `sec_type` goes on the wire as
`alp_cc3501e_wifi_connect_t::security` (`<alp/protocol/cc3501e.h>`):
**0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE**
(`CC3501E_WIFI_CONNECT_SEC_OPEN` / `_WPA2_PSK` / `_WPA3_SAE` in
`<alp/chips/cc3501e/wifi.h>`). This is a **different** encoding from the
scan-*result* security enum in that same header (`cc3501e_wifi_sec_t`),
where **1 means WEP**. This app never touches that second enum — it only
prints the scan record count — but do not carry the numbering across by
habit.

## Credentials — never committed

Build-time defines, empty defaults, this app's **own** macro prefix
(`CONNTWICE_*`) so a combined build cannot collide with
`aen-cc3501e-companion-tour`'s `TOUR_*`, `aen-cc3501e-socket-throughput`'s
`SOCKTP_*`, `aen-cc3501e-bringup`'s `CC3501E_WIFI_*`, or
`aen-cc3501e-wedge-postmortem`'s `WEDGEPM_*`:

```
west build ... -- -DEXTRA_CFLAGS="-DCONNTWICE_WIFI_SSID=\\\"myssid\\\" \
                                   -DCONNTWICE_WIFI_PASS=\\\"mypass\\\""
```

No SSID or passphrase is embedded anywhere in this file. When
`CONNTWICE_WIFI_SSID` is empty (the build default), `main()` says so and
stops before the scan or either connect attempt runs.

## What this app never does

Never `OTA_*`, never `RESET`, never a soft-AP. The only opcodes this file
issues are `WIFI_SCAN_START` (STEP 3), `WIFI_CONNECT_STA` (STEPs 4 and 5 —
which itself issues `WIFI_DISCONNECT` at entry when required, see above),
and `WIFI_STATUS` (read after each attempt). Never touches hardware beyond
the bench RAM-run this file's own header names.

## Board target

`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` — same target, overlay
memory placement, and `CONFIG_DCACHE=n` as `aen-cc3501e-command-sweep`. This
app carries no bulk throughput sweep of its own, but its PING and
`WIFI_STATUS` reads ride the identical SPI1 link every other AEN801 CC3501E
bench app measures, so it keeps the same placement rather than risk a
different link timing than the one every sibling's bench evidence was
gathered against.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow):

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-connect-twice-probe -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-connect-twice-probe/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay \
  -DEXTRA_CFLAGS="-DCONNTWICE_WIFI_SSID=\\\"myssid\\\" -DCONNTWICE_WIFI_PASS=\\\"mypass\\\""
```

RAM-run over J-Link (no MRAM programming needed — the overlay retargets
`zephyr,flash` to ITCM); read `ram_console_buf` over SWD, or the E1M edge
UART0 console if the bench has one wired (see `prj.conf`'s console toggle).
