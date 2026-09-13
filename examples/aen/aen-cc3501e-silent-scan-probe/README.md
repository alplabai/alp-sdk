# aen-cc3501e-silent-scan-probe

Bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE). Tests whether a
single `WIFI_SCAN_START` submit survives 25 seconds of a **completely
silent** host bus. Sibling of `examples/aen/aen-cc3501e-command-sweep`
(this app's bring-up template, overlay memory placement, and
`CONFIG_DCACHE=n` are copied from it) and `examples/aen/
aen-cc3501e-wedge-postmortem` (the closest sibling in intent — a
narrow, load-bearing-order probe against the same wedge family).

## The silence is the experiment

`WIFI_SCAN_START` wedges the CC3501E bridge on this bench. Root-cause
analysis has narrowed it to two readings every measurement so far is
equally consistent with:

- **B.** The firmware brackets its station role-up with a bridge quiesce,
  and the quiesce hangs. The quiesce cancels an armed callback-mode
  transfer, and the firmware's own notes record three separate bench
  occasions where doing that while the host was clocking the bus locked
  the core up. The one configuration in which it ever worked was at boot,
  with no host traffic to disrupt.
- **Other.** The heavy radio operation itself does not return within its
  bound, in which case host traffic is irrelevant.

These separate cleanly on one observation: under B, if the host puts
**nothing at all** on the bus while the firmware does its radio work, the
cancel operates on an idle armed transfer with no host bytes in flight —
the same conditions under which it historically worked — so the radio
work completes, the bridge re-initialises, and the link comes back. Under
the other reading the bus stays dead regardless of how quiet the host is.

The standard driver call (`cc3501e_wifi_scan()`) cannot test this: it
submits and then polls status every 50 ms for its whole budget, and **that
polling is the traffic under suspicion**. This app submits once through
the raw `cc3501e_request()` path instead, then goes silent.

**Any instrumentation added to this file that clocks the bus during the
silence destroys the result.** Do not add a status poll, a ping, a
diagnostic read, or any other driver call inside the `k_msleep()` that
implements it — see `src/main.c`'s own comment at that call site, which is
written to make this impossible to get wrong by accident.

## What it does, in order

1. **Bring the bridge up and confirm with a `PING`** — identical to every
   `aen-cc3501e-*` sibling's own bring-up (same `src/cc3501e_bridge.{c,h}`
   template, same overlay).
2. **Submit `WIFI_SCAN_START` exactly once**, through the raw
   `cc3501e_request()` path rather than the polling wrapper — the same
   submit-once shape `cc3501e_wifi_ap_start()` uses against `WIFI_AP_START`
   (`chips/cc3501e/cc3501e_wifi.c`, around lines 513–516), with a fixed,
   non-retried 100 ms budget (mirrors the driver's own private
   `CC3501E_REQ_TMO_MS`). Prints the exact status code. The **only**
   outcome this app treats as "the job was accepted" is the busy-style
   acknowledgement (`ALP_ERR_BUSY`, wire `RESP_ERR_BUSY`) — meaning the
   firmware's worker actually queued the scan.
3. **Then issues nothing at all for 25 seconds.** No status poll, no ping,
   no diagnostic read, nothing — not even the driver's own housekeeping,
   because there isn't any: `chips/cc3501e/` and the backends it attaches
   (`src/backends/`) carry no `k_timer`, no `k_work` / `k_work_delayable`,
   and no periodic poll of any kind (grepped for `k_timer` /
   `k_work_schedule` / `k_work_submit` / `k_work_init` /
   `K_WORK_DELAYABLE_DEFINE` / `K_TIMER_DEFINE` across both trees,
   2026-09-11 — none matched outside `src/backends/power/
   zephyr_pm_policy.c`, an unrelated PM-policy TU this app's `prj.conf`
   never pulls in). `CONFIG_ALP_SDK_CONSOLE=n` additionally keeps the
   shell thread out of existence for the same reason, even though the
   shell would talk to the E1M UART console, not this SPI1 link —
   unrequested background activity of any kind during this window is
   worth eliminating on principle.
4. **Then a single `PING`.** The first thing to touch the bus since the
   STEP 2 submit. Reported plainly: did the link survive the silence.
5. **Then a normal scan through the ordinary wrapper**, `cc3501e_wifi_scan()`
   with a 40000 ms budget. If the role is latched from step 2, this second
   scan performs no role-up and should return records promptly. Prints the
   record count, and for each record the decoded security name (`open` /
   `wep` / `wpa` / `wpa2` / `wpa3`), signal strength in dBm, and channel.

Finally prints a plain **READING**: whether the link survived the silence,
and whether the second scan returned records, plus which of the two
mechanisms above the combined result is consistent with.

## The one way this run is VOID

If step 2's submit does not get its busy-style acknowledgement — an I/O
error or a timeout instead — the submit frame itself did not complete, and
25 seconds of silence afterwards would be silence around **nothing**,
proving neither reading above. This app checks for exactly that and prints
`=== VOID RUN ===` before stopping, rather than continue into a silence
whose premise already failed.

## No credentials

A scan takes no SSID or passphrase, and that is part of why it is the
right probe: this app needs no build-time credential defines and commits
none — unlike `aen-cc3501e-wedge-postmortem`'s `WIFI_CONNECT_STA` repro,
there is nothing here to redact.

## Board target

`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` — the same target, overlay
memory placement, and `CONFIG_DCACHE=n` as `aen-cc3501e-command-sweep`, for
the same reason: this app's own submit and both `PING`s ride the exact
SPI1 link every AEN801 CC3501E bench app measures, and this app's own
25-second silence is sensitive to exactly the same transmit-FIFO refill
timing that link's overlay documents — running faster (cache-on/DTCM) than
every other AEN801 bench app would not be a comparable probe.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow), same shape
as its `aen-cc3501e-*` siblings:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-silent-scan-probe -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-silent-scan-probe/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

RAM-run over J-Link (no MRAM programming needed — the overlay retargets
`zephyr,flash` to ITCM). The default build's primary read path is the E1M
edge UART0 console (`CONFIG_UART_CONSOLE=y` in `prj.conf`) — connect to it
directly. Only for a serial-less bench: comment out the four
`CONFIG_SERIAL`/`CONFIG_CONSOLE`/`CONFIG_UART_CONSOLE`/
`CONFIG_UART_INTERRUPT_DRIVEN` lines and uncomment the three
`CONFIG_RAM_CONSOLE*`/`CONFIG_UART_CONSOLE=n` lines `prj.conf` already
carries, then read `ram_console_buf` over SWD instead.

**Expected duration:** dominated by the 25-second silence itself. With a
healthy first `PING` and a prompt STEP 5 scan, roughly 30–40 seconds total
through the `=== READING ===` block. A `VOID RUN` stops within a couple of
seconds of the STEP 2 submit, well before the silence would have started.

## What this app does NOT do

It never polls, pings, or reads anything during the 25-second silence —
that is the entire point, see "The silence is the experiment" above. It
never widens the STEP 2 submit's timeout hunting for a different
acknowledgement, and never retries the submit. It never modifies
`aen-cc3501e-command-sweep` or any other sibling. It carries no Wi-Fi
credentials and needs none. It does not attempt to fix the wedge; it only
tests one specific condition under which it might not happen.
