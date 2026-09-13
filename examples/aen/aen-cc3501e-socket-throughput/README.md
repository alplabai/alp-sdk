# aen-cc3501e-socket-throughput

Bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE). Measures the
CC3501E Wi-Fi 6 / BLE 5.4 coprocessor's **end-to-end TCP socket
throughput** — Wi-Fi association through `cc3501e_sock_recv()` — over a
window sized so the measurement is actually trustworthy. Sibling of
`examples/aen/aen-cc3501e-command-sweep` (this app's structure, bring-up
template, overlay memory placement, and `CONFIG_DCACHE=n` are copied from
it) and `examples/aen/aen-cc3501e-companion-tour` (this app's
build-time-credential convention is copied from it).

## What this measures, and why it exists

A previous sweep (`aen-cc3501e-command-sweep`'s Part 2) reported
`STREAM_WRITE` throughput as **104 KB/s at 64 bytes** and **250 KB/s at 256
bytes**. Those numbers were never rates — they were an artifact of the
instrument.

That sweep timed each transfer with `k_uptime_get()`, which ticks in whole
milliseconds, and it timed five 256-byte transfers — about 255
microseconds each on a link running near 980 KB/s. Every one of those five
transfers landed **at or below the timer's own tick**: the sweep's own
min/max columns read `min=1 max=1` (millisecond), which is the timer's
resolution floor, not the transfer's real duration. Dividing a byte count
by a millisecond-quantized total makes the reported rate rise with
transfer size whether or not per-frame overhead is actually amortizing —
and the arithmetic proves it exactly: 256 bytes at a real ~980 KB/s takes
~255 µs, which a 1 ms-tick clock rounds up to 1 ms, and
`256 B / 1 ms = 250 KB/s` — precisely the number that sweep reported. The
four-fold gap between 250 KB/s and the link's real ~980 KB/s was entirely
the instrument, not the link.

The repo's own recorded numbers agree in magnitude with the ~980 KB/s
figure: the CC3501E bridge firmware's `hal/ti/cc3501e_hw_ti_sock.c`
documents **"end-to-end HTTP over the bridge: 8 KB = ~660 kB/s,
16 KB = ~730 kB/s, 64 KB = ~742 kB/s"** — climbing toward ~980 KB/s as the
transfer amortizes more of its fixed per-frame cost over more data, the
shape a genuine rate has and a timer-floor artifact does not.

That same firmware file also documents the fix, and it is the pattern this
app copies: publish a **windowed** rate, accumulating at least 131072
bytes (128 KiB) before dividing **once**. Its own comment explains why a
naive per-transfer average is worse than useless: *"A cumulative average
from socket-open decays toward zero as soon as the transfer finishes and
the socket goes idle, which is what made the first attempt read 12 kB/s on
a link doing far more."* Measure a big window, divide once — this app is
that pattern, applied end-to-end from the host side of the link instead of
the coprocessor's own radio-only loopback.

### Why the window is at least 4 MiB

`SOCKTP_BYTE_BUDGET` in `src/main.c` defaults to 4 MiB. At the link's real
~980 KB/s, that takes about 4 seconds — turning `k_uptime_get()`'s 1 ms
tick into a **~0.02% error on the total**, instead of the **4x error** a
single millisecond-quantized 256-byte transfer produced above. Do not
shrink this budget back toward the noise floor; that is exactly the
mistake this app exists to not repeat.

## What it does

1. **STEP 1** — bring the bridge up (`cc3501e_bridge_bringup()`,
   byte-identical to `aen-evk-demo` phase 8 and every `aen-cc3501e-*`
   sibling).
2. **STEP 2** — confirm the link answers (`PING`, retried).
3. **STEP 3** — associate to Wi-Fi. **When no credentials are configured,
   this step selects bridge-only mode instead** (see "Bridge-only mode"
   above) and the app returns after that sweep — see Credentials below.
4. **STEP 4** — open a TCP socket, connect it to the configured server,
   send a plain HTTP GET, then read the response **body** in a loop
   (skipping past the HTTP headers first — they are a fixed few-hundred-
   byte cost unrelated to the link's own rate) until the transfer
   completes or the byte budget is reached. The whole window is timed with
   **one** `k_uptime_get()` pair, clock started on the first body byte
   (not on connect or on the request being sent — the same reasoning the
   bridge firmware's own radio-speedtest loop uses: the gap before the
   first segment arrives is idle time that has nothing to do with the
   rate).

A progress line prints roughly every 512 KiB with a running rate, so a
stalled transfer is visible on the console instead of looking like a hang.

## What it reports

Every figure printed on its own line, so the arithmetic
(`bytes * 1000 / elapsed_ms == rate`) can be checked by hand:

- total body bytes received (the timed figure the rate is derived from)
- total bytes off the wire, header included (a separate diagnostic figure)
- elapsed milliseconds (the one window, one clock)
- derived rate in both B/s and KB/s
- number of `cc3501e_sock_recv()` calls made, and the mean bytes per call
  that actually carried data — this separates "the link is slow" from "the
  host is making too many small calls" for the same total transfer.

## Recv sizing

`cc3501e_sock_recv()` clamps whatever `cap` this app requests down to its
own internal ceiling
(`ALP_CC3501E_MAX_PAYLOAD - sizeof(alp_cc3501e_sock_recv_resp_t) - 1`,
computed in `chips/cc3501e/cc3501e_sockets.c` — 4096 − 24 − 1 = **4071
bytes** as of protocol v5) regardless of what this app passes. That
internal bound is already tighter than the generic wire-MAJOR-4
CRC-adjusted ceiling (`ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_CRC_BYTES` =
4094), the same relationship `aen-cc3501e-command-sweep`'s own
`sweep_crc_headroom()` documents for `STREAM_WRITE`: the wrapper already
applies the tighter bound itself, so no further CRC-headroom subtraction
belongs on top of it here. `SOCKTP_RECV_CEILING` in `src/main.c` computes
this ceiling from the **public** `alp_cc3501e_sock_recv_resp_t` struct
(not the driver's private size constant), and this app requests exactly
that — not a round number like 256 or 1024 — so every `SOCK_RECV` call
moves the largest chunk the wrapper will ever hand back in one round trip.
A smaller cap does not fail; it just adds avoidable round trips for the
same total bytes, which is exactly the per-frame overhead this
measurement exists to not hide.

## Bridge-only mode — a number without Wi-Fi

The socket path above needs a live Wi-Fi association and a server that
answers. As of this writing that association has not completed on the
bench, which has left this app unable to produce a socket figure at all.
So: when no credentials are configured, this app now measures the **one
segment that IS reachable without them** — the host-to-CC3501E SPI link
itself, driven with `STREAM_WRITE`, with no radio, no association, no IP
stack, and no server involved.

**Mode selection reuses the existing SSID-empty condition, rather than a
new build-time define.** STEP 3 already treats an empty `SOCKTP_WIFI_SSID`
as "nothing downstream can run"; this app now spends that same idle time on
the bridge-only sweep instead of returning immediately. A credential-free
build therefore measures the bridge; a credentialed build still measures
end-to-end, unchanged. The two conditions are already mutually exclusive on
the one knob (no credentials means no end-to-end path exists to compare
against anyway), so a second define would only add a way for the two modes
to disagree with each other for no benefit.

**This figure is NOT comparable to an end-to-end HTTP figure — say so
plainly.** The bridge-only sweep measures one link segment (host SPI →
CC3501E) with no radio and nothing downstream of it. It is not, and must
never be read as, a substitute for the end-to-end number STEP 4 measures
when credentials are set. The bridge firmware's own
`hal/ti/cc3501e_hw_ti_sock.c` records that end-to-end figure directly:
**"end-to-end HTTP over the bridge: 8 KB = ~660 kB/s, 16 KB = ~730 kB/s,
64 KB = ~742 kB/s"** — that is the end-to-end number, roughly 660 to
742 kB/s. Whatever the bridge-only sweep below reports is a different
measurement of a different, smaller piece of the path.

### What it sweeps, and the trap it avoids

`STREAM_WRITE` payload sizes **64, 128, 256, and 512 bytes**, each over its
own **1 MiB window** — well above the 128 KiB floor this app's own
`SOCKTP_BYTE_BUDGET` reasoning derives (see "Why the window is at least
4 MiB" above; the same millisecond-granularity artifact applies to
`STREAM_WRITE` timed per-call, which is exactly how the earlier 104 KB/s
(64 B) and 250 KB/s (256 B) figures were produced in the first place). 1 MiB
was chosen over the 128 KiB floor because it is cheap — about a second per
size at the link's real rate — and pushes the same quantisation error down
another order of magnitude.

**The sweep never goes past 512 bytes, and this is a hard constraint, not a
style choice.** Measured, reproducible 3 of 3 on bench: 64 B and 256 B
`STREAM_WRITE` calls pass 5 of 5, while 1024 B and 4092 B (the wrapper's
own ceiling) both fail with `rc=-5` and the link does not recover for the
rest of that run. 512 B has since been measured clean 4 of 4 on this
bench, but the ceiling stays where it is: nothing above it has ever
completed.
Sizes run **ascending**, and the sweep reports each size **the instant it
completes** — so a wedge partway through still leaves every already-
completed size's real numbers on the console. On the first `STREAM_WRITE`
failure the sweep stops immediately, prints which size failed, and does
**not** attempt the next size or report it as data.

Per completed size it prints total bytes sent, elapsed milliseconds, the
derived rate in B/s and KB/s, the `STREAM_WRITE` call count, and the mean
bytes per call — each as a separate figure so the arithmetic can be checked
by hand — followed by a running progress line every 128 KiB so a stall is
visible rather than looking like a hang. After the sweep, it prints whether
the rate curve **flattens** as size grows (per-frame overhead amortizing
less each step) — the question the original artifact-only sweep existed to
answer and could not.

## Credentials — never committed

Follows the convention `aen-cc3501e-companion-tour` establishes: Wi-Fi
credentials and the throughput target arrive as **build-time defines**,
with **empty/placeholder defaults**, under this app's own macro names
(`SOCKTP_*`, distinct from the tour's `TOUR_*`, so the two examples never
fight over a shared `-D` on a combined build). **No real SSID,
passphrase, or server address is embedded anywhere in this repository,
including this file** — every value below is an obvious placeholder.

```
west build ... -- -DEXTRA_CFLAGS="\
  -DSOCKTP_WIFI_SSID=\\\"myssid\\\" \
  -DSOCKTP_WIFI_PASS=\\\"mypass\\\" \
  -DSOCKTP_SERVER_A=192 -DSOCKTP_SERVER_B=168 \
  -DSOCKTP_SERVER_C=1   -DSOCKTP_SERVER_D=50 \
  -DSOCKTP_SERVER_PORT=8000 \
  -DSOCKTP_HTTP_PATH=\\\"/bigfile.bin\\\" \
  -DSOCKTP_WIFI_SECURITY=1"
```

`SOCKTP_WIFI_SECURITY` selects the association type: `0` open, `1` WPA2-PSK
(the default), `2` WPA3-SAE. It is listed here because it was previously
undocumented, which meant an operator reading only this file could not set it
at all — and on a WPA3 access point the default silently picks the wrong one.

Note that on a bench with a WPA3 AP, selectors `1` and `2` have been observed
producing identical results, so this is not the first thing to vary when an
association fails. `SOCKTP_SERVER_A` through `_D` are four separate integer
octets rather than a host string; passing a `SOCKTP_SERVER_HOST="1.2.3.4"`
compiles cleanly as an unused macro and silently targets the default address
instead, which has already cost one bench run.

When `SOCKTP_WIFI_SSID` is empty (the build default), the app brings the
bridge up, confirms `PING`, prints that the networked steps are skipped,
and returns — no radio or socket call is ever made. `SOCKTP_SERVER_*`
default to the `.1` gateway of a generic `192.168.1/24` bench LAN on port
80, path `/` — a placeholder, not a real bench server; point it at a host
that actually answers with a large body (`SOCKTP_HTTP_PATH`) before
trusting a result.

## Board target

`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` — the same target
`aen-cc3501e-command-sweep` uses, including its SRAM0 system-RAM placement
and `CONFIG_DCACHE=n`. That placement is **load-bearing**, not decorative:
the overlay's own header explains that this app's `SOCK_RECV` window rides
the exact SPI1 link every AEN801 CC3501E bench app measures, sensitive to
cache-on vs. cache-off timing on this silicon (a DesignWare SSI master
that underruns its TX FIFO deasserts its own chip-select mid-frame) —
running this app's read loop from a faster cache-on/DTCM path than every
other AEN801 bench app that has measured this link would produce a number
nobody else's run is comparable to.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow), same shape
as `aen-cc3501e-command-sweep`:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-socket-throughput -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>;<fatfs>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-socket-throughput/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

`<fatfs>` is only needed because `west list` in some worktree checkouts has
no `fatfs` entry even though the module is on disk — include it if your
build complains about a missing FatFs module; it is otherwise inert for
this app (it does not touch storage).

RAM-run over J-Link (no MRAM programming needed — the overlay retargets
`zephyr,flash` to ITCM); read `ram_console_buf` over SWD, or the E1M edge
UART0 console if the bench has one wired (see `prj.conf`'s console
toggle).

## What this app does NOT do

It never modifies `aen-evk-demo`, `aen-cc3501e-command-sweep`, or
`aen-cc3501e-companion-tour`. It never flashes anything and never touches
the bench beyond a RAM-run — build and run it against a board already
under your own bench session.
