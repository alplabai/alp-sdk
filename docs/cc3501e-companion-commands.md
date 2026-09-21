<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Alp Lab AB -->

# CC3501E companion command + API reference

The on-module **TI CC3501E** Wi-Fi 6 + BLE 5.4 coprocessor is driven from
the Alif host over the inter-chip SPI bridge. Application firmware should use
the portable Wi-Fi and BLE APIs first:

- [`<alp/iot.h>`](../include/alp/iot.h) — `alp_wifi_open()`,
  `alp_wifi_connect()`, `alp_wifi_disconnect()`.
- [`<alp/ble.h>`](../include/alp/ble.h) — `alp_ble_open()`, scan,
  advertise, connect, and GATT operations.

On E1M-AEN builds, those dispatchers select the exact CC3501E backend after
the application bring-up helper attaches its live
[`cc3501e_t`](../include/alp/chips/cc3501e.h) handle. The lower-level
`cc3501e_*` API and the `alp companion` shell remain the diagnostics and
bring-up surface for firmware-version, raw scan records, sockets, OTA, and
bridge health. Two diagnostic ways in:

- **Interactively** — the `alp companion` command tree on the Zephyr shell
  (the companion console backend under `src/zephyr/console/`). This page is
  its reference.
- **From firmware diagnostics** — the `cc3501e_*` C API directly (see the
  [`aen-cc3501e-companion-tour`](../examples/aen/aen-cc3501e-companion-tour)
  example, which walks the portable checkpoint and the diagnostic surface in
  sequence).

> The same `alp companion` group binds the **GD32** supervisor on V2N SoMs
> (`CONFIG_ALP_SDK_V2N_SUPERVISOR`, plus a non-negative
> `CONFIG_ALP_SDK_V2N_SUPERVISOR_SPI_BUS_ID` or
> `CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID` — both default `-1`, which no
> in-tree board overrides yet (tracked in #2044)) instead of the CC3501E;
> there it exposes `companion gpio read/write` rather than the Wi-Fi/BLE
> tree below. This page documents the **CC3501E (Alif)** binding.

---

## Enabling the console

```kconfig
CONFIG_ALP_SDK_CONSOLE=y               # the alp command tree
CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION=y # the `alp companion` group
CONFIG_SHELL=y                         # Zephyr shell backend
```

The application registers its live CC3501E handle once at boot via
`alp_console_companion_set()` (after `cc3501e_bridge_bringup()`); until then the
commands report the bridge is not ready. See
[`console.md`](console.md) for the console's safety tiers
(`CONFIG_ALP_SDK_CONSOLE_UNSAFE` gates the write-side commands).

---

## Meta

| Command | What it does |
|---|---|
| `alp companion ver` | Read the companion firmware / protocol version. |
| `alp companion ping` | Liveness round-trip (cheapest is-it-alive probe). |
| `alp companion reset` | Soft-reset the CC3501E firmware in-band (the link drops; re-sync after). |
| `alp companion bench [n]` | Time `n` `GET_VERSION` round-trips over the bridge. |
| `alp companion recover` | Warm-reset the bridge on demand (see "Link auto-recovery" below); refuses while an OTA session is open; prints the recovery count. |
| `alp companion linklog` | Dump the link-failure ring, oldest entry first (see "Link-failure ring" below). |

## Link auto-recovery (issue #2126)

An AEN EVK bench unit has been observed to wedge the bridge link mid Wi-Fi-connect
roughly 3 times in 50 connects, for a cause that isn't fully identified yet
(firmware-side self-heal is tracked separately,
cc3501e-bridge-firmware#142). Once wedged, every request fails (`ver` → `-4`
or `-5`) until the board is power-cycled — except a warm `nRESET`
(`cc3501e_recover()`) has recovered every observed wedge so far (#1691).

`cc3501e_link_check_and_recover()` wires that warm reset into the driver's
own failure exits (`poll_by_repeat()`'s terminal return and
`cc3501e_wifi_connect()`'s own timeout exit), so an application does not have
to detect and recover a wedge itself:

1. A top-level op returns `ALP_ERR_IO`/`ALP_ERR_TIMEOUT` with **no reply
   status ever decoded** off the wire (the same signal the transport uses
   internally to tell a real device-side error apart from silence).
2. Up to 24 bare `PING`s, 500 ms apart (~12–18 s), check whether the link is
   merely in one of the transport's known transient windows (a radio-down
   window, a teardown/re-arm race) rather than genuinely dead. The probe
   stops at the first answer. It is deliberately longer than the bridge's
   own 10 s radio-down window, so a busy-but-healthy bridge is never reset.
3. Only if every probe fails does it warm-reset (`cc3501e_recover()`),
   re-confirm the firmware's protocol version, and clear the driver's
   same-context busy latches — the Wi-Fi association, BLE host, and any open
   sockets are gone either way, exactly as after a manual `alp companion
   recover` or a `cc3501e_recover()` call.

Guards: never while an OTA/update session is open (the device is
deliberately deaf for 22–41 s of slot erase there, and a reset would abort
the session); a cooldown between attempts on the same link, starting at 30 s
and doubling to 5 minutes while attempts keep failing, back to 30 s once one
succeeds; only one recovery at a time, holding the link's request lock from
the reset through the confirming `PING`; and
`CONFIG_ALP_SDK_CC3501E_AUTO_RECOVER` (default `y`) to turn the whole
mechanism off on a bench that wants to observe a wedge rather than have the
driver clear it. The measurement apps `aen-cc3501e-wedge-postmortem`,
`aen-cc3501e-command-sweep` and `aen-cc3501e-silent-scan-probe` set it to
`n` for that reason.

A successful recovery logs `cc3501e: link recovered by warm reset (#n)` and
bumps `ctx->recover_count`, which a host application can read directly for
its own telemetry, or subscribe to with `cc3501e_set_recover_callback()`.

`alp companion recover` runs the same `cc3501e_recover()` the automatic path
does, with the same OTA-session refusal, the same one-at-a-time rule and the
same shared cooldown timestamp — it just skips the probe, for an operator
who already knows the link needs it. It bumps the same `recover_count`.

After any recovery the bridge has rebooted: the Wi-Fi association, the BLE
host and every open socket are gone. Socket handles minted before it now
carry the previous link epoch in their upper byte and fail closed with
`ALP_ERR_NOT_READY`; open new ones. A proxied GPIO pin configured before the
recovery answers `ALP_ERR_NOT_READY` until it is configured again.

## Link-failure ring (issue #2136)

On a bridge wedge, the only usable evidence used to be discarded the moment
the next request ran: `cc3501e_request_locked()` overwrites
`ctx->rx_scratch[0]` at its `out:` label on every pre-decode failure.
Firmware-side capture cannot substitute here — a wedge-probe build's warm
`nRESET` (the same reset `cc3501e_recover()` pulses to clear the wedge) does
**not** leave the firmware's retained RAM intact across the reset
(cc3501e-bridge-firmware#148: `boots`/reset-cause read identically before and
after, i.e. a fresh power-on-style boot, not a survived snapshot).

`ctx->link_log` (`<alp/chips/cc3501e/core.h>`) is a fixed 8-entry ring, kept
on the host side for exactly this reason. `cc3501e_request_locked()` records
one entry only when a pre-decode transport/framing failure leaves no reply
status decoded — never on success, never on a genuine decoded device error.
Each entry carries: a timestamp; the opcode; which of the four wire phases
the attempt reached; the mapped `alp_status_t`; the request-header phase's 4
MISO bytes and the reply-header phase's 4 bytes; READY-line evidence
(sampled before the request and again at exit, plus whether the line has
ever proven itself wired this boot); and the recovery attempt count at
record time. A consecutive-failure counter (`ctx->link_log_fail_streak`,
read via `cc3501e_link_log_fail_streak()`) resets on the next decoded reply,
and the last successful `GET_DIAG_INFO` probe (`ctx->link_log_last_probe_word`
/ `_last_probe_ms`) is kept alongside for correlation — a wedge-probe
firmware build rides its own free-running word in that reply's
`free_heap_bytes` field.

**A byte array is meaningless unless its VALID flag is set** (#2136 review).
`hdr_bytes` / `reply_hdr` used to be `memcpy`'d from `ctx->rx_scratch`
regardless of whether that phase's own transceive actually succeeded — on a
bail, that could capture STALE SCRATCH left by a completely different,
earlier exchange (the poisoned-marker byte plus residue) and present it as
if it were this attempt's wire data, producing a confident misdiagnosis.
Each byte array now has a matching `entry.flags` bit —
`CC3501E_LINK_LOG_HDR_BYTES_VALID` (`0x8`) for `hdr_bytes`,
`CC3501E_LINK_LOG_REPLY_HDR_VALID` (`0x10`) for `reply_hdr` — set only when
that phase's transceive returned `ALP_OK`; unset means the array is all-zero
and carries no information (either the attempt never reached that phase, or
it reached it and the transceive itself failed). **Check the VALID bit
before reading either array**, including before applying the classification
below.

Read the ring with `cc3501e_link_log_count()` / `cc3501e_link_log_get()`
(both take the driver's internal request lock, so a read can never observe a
write mid-entry), or `alp companion linklog`, which dumps it as hex, oldest
entry first, with a legend line. The automatic and manual recovery paths
(`companion_recover_notify()`, `src/zephyr/console/alp_console_companion.c`)
also dump it right before printing their own recovery line, so a bench run
captures the ring around a reset with no extra step — using
`cc3501e_link_log_recover_streak()` for the fail-streak field on that dump,
not the live `cc3501e_link_log_fail_streak()`: the recovery's own confirming
PING already reset the live streak to 0 by the time the dump runs.

**The ring survives the very recovery it exists to outlive** (#2136 review).
`cc3501e_link_check_and_recover()`'s own probe fires up to 24 PINGs before
concluding the link is dead — more than the ring's 8 slots, so an
unsuppressed probe would guarantee-evict the ORIGINAL wedging op's entry,
replacing it with 8 identical PING failures and destroying the one thing an
operator actually needed. The probe now sets `ctx->link_log_suppress` for its
own duration; `cc3501e_request_locked()`'s ring-write site checks it and, when
set, counts the failure into `ctx->link_log_probe_fail_count`
(`cc3501e_link_log_probe_fail_count()`) instead of writing a ring entry — the
probe's own failures stay countable (the recovery-notify dump prints them)
without ever evicting the wedging op's own evidence.

**A ring this thin can still mean "many failures", not "few"** (#2136
review). A request that loses `cc3501e_lock_acquire()` returns
`ALP_ERR_BUSY` after the 100 ms
`CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS` window without ever entering
`cc3501e_request_locked()` — so it leaves no ring entry and does not bump
`link_log_fail_streak` either. During a wedge that is the common shape for
every thread except whichever one is already holding the lock; do not read a
thin ring as proof of a mild fault.

Roughly, the classification the ring makes possible — **only once the
relevant VALID bit confirms the bytes are real**:

- **deaf-armed** — `hdr_bytes` is VALID and reads the armed marker
  (`ALP_CC3501E_SYNC_IDLE` x4) every attempt, `reply_hdr` is ALSO VALID
  (the reply-header transceive itself succeeded) but never echoes the
  opcode, and that non-echo repeats: the slave armed the link and then
  never dispatched anything.
- **desynced** — `hdr_bytes` is VALID and reads something other than the
  armed marker (e.g. `0x00`, the slave's payload-phase dummy, or stale reply
  bytes), and the pattern changes across attempts as the slave consumes
  bytes the host keeps clocking.
- **crashed / not driving** — `hdr_bytes` VALID with a constant `0xFF` or
  `0x00` run and no variation, with a frozen READY reading.
- **transceive itself failing** (a genuine transport/IO fault, not a
  framing issue) — the relevant VALID bit is UNSET and the byte array reads
  all-zero; do not classify from an unset-VALID entry at all, it carries no
  wire evidence.

## `alp companion wifi`

| Command | What it does |
|---|---|
| `wifi scan` | Scan for Wi-Fi APs and list SSID / channel / RSSI / security. |
| `wifi connect <ssid> [pass] [wpa3]` | Associate as a station (omit `pass` for open; `wpa3` selects SAE). On a failed (or timed-out) result, the line also prints `reason: <N>` when the bridge recorded one for this attempt -- see below for what `<N>` means. |
| `wifi disconnect` | Tear down the STA association. |
| `wifi ap <ssid> [pass] [wpa3]` | Start a soft-AP (omit `pass` for an open AP). |
| `wifi ap-stop` | Stop the soft-AP. |
| `wifi status` | Show connection state + RSSI + IP. RSSI is a live radio read when connected -- can take ~10s (~20s if the link is wedged). On a failed connect, also prints `reason: <N>` when the bridge recorded one. |

**What `reason: <N>` means.** `<N>` is the reason/status code for the MOST
RECENT connect attempt: the low byte of the 802.11 REASON code from a
DISCONNECT event, or the 802.11 STATUS code from an ASSOCIATION_REJECTED /
AUTHENTICATION_REJECTED event -- two different code tables, and nothing on
the wire says which one, so `fail: 2` (REJECTED) does not tell you which
table `<N>` came from. It is recorded only while that attempt was
CONNECTING, then frozen and **persists through later state publishes** --
including a later `wifi disconnect`, which republishes this same frozen
value rather than clearing it -- until the next connect attempt starts. A
successful connect (`CONNECTED`) always publishes `0`, even over an earlier
transient rejection in the same attempt that a firmware-internal retry then
overcame. `0` means nothing was recorded for that attempt, not "no cause": a
clean success or a bare timeout also reads `0`. It never holds vendor reason
200 (`WLAN_DISCONNECT_USER_INITIATED`). Known residual: a late event from the
PREVIOUS attempt landing in the brief window right before the new attempt's
own connect call can still be recorded against the new one.

`wifi connect`'s printed `reason: <N>` is only ever the CURRENT attempt's own
failure: the console checks that the fetched status latch's `state` itself
reads `CONN_FAILED` before trusting `<N>` -- a plain `timed out` with the
latch still `DISCONNECTED`/`CONNECTING` (the submit was bounced busy by a
concurrent worker op, or lost to a transport fault) never got far enough to
record anything of its own, and printing a leftover value there would blame
an unrelated earlier attempt.

`wifi ap` submits `WIFI_AP_START` once, then confirms the outcome against an
independent channel: it polls `GET_DIAG_INFO`'s role field until the role
reports `WIFI_AP` (prints `ap "<ssid>" up (...)`) or the connect budget
elapses (prints `ap start "<ssid>" failed (-4) (not confirmed within the
budget)`). The submit itself is never retried — see `cc3501e_wifi_ap_start()`
for why a retry loop around this opcode is provably unwinnable.

## `alp companion ble`

| Command | What it does |
|---|---|
| `ble enable` | Bring up the BLE controller + NimBLE host. |
| `ble disable` | Tear the BLE controller + host back down. |
| `ble scan` | Scan for advertisers (needs `ble enable` first). |
| `ble scan-stop` | Stop an in-progress scan. |
| `ble adv` | Start connectable advertising (fixed interval). |
| `ble adv-stop` | Stop advertising. |
| `ble connect <aa:bb:cc:dd:ee:ff> [random]` | Central-connect to a peer (`random` = random address type). |
| `ble disconnect` | Drop the active BLE connection. |
| `ble gatt register <hexbytes>` | Register an opaque GATT attribute table. |
| `ble gatt read <handle>` | Read a GATT attribute value. |
| `ble gatt write <handle> <hexbytes>` | Write a GATT attribute value. |
| `ble gatt notify <handle> <hexbytes>` | Send a GATT notification. |

## `alp companion diag`

| Command | What it does |
|---|---|
| `diag info` | Firmware version / reset cause / uptime / active role / free heap / lwIP DHCP state / netif up-link-tries. |
| `diag stats` | Frame counters (frames answered OK / with an error). |
| `diag loglevel <0..255>` | Set the firmware log verbosity. |

## `alp companion sock`

| Command | What it does |
|---|---|
| `sock tcp-get <ip> <port> <path>` | Open a TCP socket, issue an HTTP/1.0 `GET`, and print the reply. |
| `sock serve <port> [seconds]` | Bind and listen on `<port>`, then answer each inbound HTTP request for `[seconds]` (default 60). |

The socket primitives underneath these — `open`, `connect`, `bind`, `listen`,
`send`, `recv`, `close` — are available from firmware as the `cc3501e_sock_*`
API (the console exposes only the two composed helpers).

`sock serve` is the **serving** direction, added in wire protocol v9: it is how
a product with no Ethernet PHY puts a web console on the module's own soft-AP.
Bring the AP up first (`wifi ap <ssid> <pass>`), then run `serve`; it prints the
AP-side address to aim a client at. There is no `accept` command and no
accept opcode on the wire — `accept()` blocks, and this bridge is strict
request/reply lockstep, so an inbound connection is delivered as an
`EVT_SOCK_ACCEPTED` event on the polled event queue instead, carrying a handle
the host then uses with the ordinary `recv` / `send` / `close`.

Two things to expect from `serve`:

- **The shell is blocked for the whole window.** Zephyr's shell has no
  cancellation hook a running command can poll, so ctrl-c does not cut it
  short; pick a `[seconds]` you are willing to wait out.
- **The host owns every accepted handle.** The firmware never closes one on the
  host's behalf, so a serve loop that forgets `cc3501e_sock_close` leaks
  firmware sockets until the IP stack runs out.

## `alp companion spi1`

The E1M connector's SPI1 pins land on the CC3501E, not the Alif — a carrier
device on that bus is reached by **relay**: the CC3501E is the SPI
controller and these verbs hand it the bytes over the inter-chip bridge (see
[`cc3501e-bridge.md`](cc3501e-bridge.md)). CONFIGURE must succeed once per
console session before TRANSFER; a session that skips it gets `NOT_READY`
without anything touching the wire.

| Command | What it does |
|---|---|
| `spi1 configure <freq_hz> <mode 0..3> <cs 0\|1>` | Acquire the SPI1 controller; print the reply's SCK and per-chunk cap. |
| `spi1 xfer <hexbytes> [hold] [norx]` | Full-duplex chunk: clock the given bytes, print the RX bytes (`hold` leaves CS asserted, `norx` discards MISO). |
| `spi1 read <len> [hold] [<fill>]` | The NO_TX half: clock `len` copies of `fill` (default `0xFF`) out, print what came back. |
| `spi1 release` | Deassert CS, close SPI1, free the bus — the escape hatch, never fails. |

`spi1 configure`'s printed SCK can read `sck unknown actual (...)` rather
than a Hz value: the TI backend has no clock-divider read-back yet, so it
reports the honest "not measured" answer instead of echoing the request as
if it were a silicon measurement.

The `cc3501e_spi1_configure` / `_transfer` / `_release` API underneath these
verbs is documented in
[`<alp/chips/cc3501e/core.h>`](../include/alp/chips/cc3501e/core.h).

---

## Host-driver-level surfaces (console + `cc3501e_*` API)

Two companion subsystems are driven both interactively and programmatically:

### OTA firmware update (`alp companion ota` / `cc3501e_ota_*`)

Stream a signed CC3501E vendor image over the bridge into the coprocessor's
non-primary slot, which it then swaps on reboot (PSA-FWU). The
`alp companion ota` shell group drives a session interactively:
`ota status` reports the session state and progress cursor, `ota begin
<total_len_bytes>` starts one (the image bytes are pushed by the firmware
path below, not typed into the shell), and `ota abort` cancels it.

| API | What it does |
|---|---|
| `cc3501e_ota_update(ctx, image, len, tmo)` | Full cycle: `BEGIN` → chunked `WRITE` → `FINISH`. |
| `cc3501e_ota_begin` / `_write` / `_finish` | Granular streaming controls (`tcp-get`-style composition). |
| `cc3501e_ota_abort` | Cancel an in-flight session (back to IDLE). |
| `cc3501e_ota_status` | Session state + bytes-written cursor + declared total (for resume). |

See [`cc3501e-bridge.md`](cc3501e-bridge.md) "OTA" and
[`cc3501e-production.md`](cc3501e-production.md).

### GPIO proxy — via the portable `<alp/gpio>` API

The CC3501E fronts a set of E1M IOs (IO11 / IO13 / IO15..IO21) and the two
camera-enable LDOs. With `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y` and a populated
route table (`cc3501e_gpio_routes[]`), `alp_gpio_open(ALP_E1M_GPIO_IOxx)` for a
mapped IO routes over the bridge while every other pin delegates to the
platform GPIO driver — the application code is identical either way. The raw
`cc3501e_gpio_configure` / `_write` / `_read` / `_set_interrupt` +
`cc3501e_cam_enable` / `cc3501e_power_policy` calls are also available directly.
See [`cc3501e-gpio-bench.md`](cc3501e-gpio-bench.md).
