# aen-cc3501e-wedge-postmortem

Bench app for the E1M-AEN801 (Alif Ensemble E8, M55-HE). Reproduces the
CC3501E `WIFI_CONNECT_STA` wedge and captures forensic evidence **while the
link is still down**, before any recovery is attempted. Sibling of
`examples/aen/aen-cc3501e-socket-throughput` (this app's structure, bring-up
template, overlay memory placement, and `CONFIG_DCACHE=n` are copied from
it) and `examples/aen/aen-cc3501e-companion-tour` (this app's
build-time-credential convention is copied from it).

## Why this app exists

Issuing `WIFI_CONNECT_STA` over the CC3501E SPI bridge wedges the link. A
`PING` one second earlier answers on the first attempt, and the connect
itself gets no reply at all. Reproduced three times out of three.

An earlier version of this file said the link then stayed dead through a
host-driven reset pulse, and that only a full carrier power cycle revived
it. **That was wrong and is retracted.** It came from a run where a fresh
Flow C image was loaded over an already-running app, which double-faults the
host core into lockup before a single console byte is emitted (`clearing
lockup after double fault`, `pc: 0xeffffffe`). The host core was dead, not
the link. A later run from a verifiably cleared buffer had a warm `nRESET`
pulse revive the link on the first attempt. Cold-cycle between Flow C runs,
or this trap reads exactly like a dead bridge — it has now done so twice.

Three mechanisms are still live, and this app exists to separate them:

1. A task inside the CC3501E firmware is hung — the M33 has stopped
   servicing SPI, but the chip is otherwise alive.
2. The host's own reset sequence never actually reboots the chip, so every
   "reset did not revive it" observation proves nothing.
3. The wedge is outside CC3501E firmware state entirely — the Alif SPI1 or
   DMA side, or a CC3501E always-on power domain — in which case no
   firmware change can fix it.

## What it does, in order (the order is load-bearing)

**PHASE A — positive control, in a healthy session, before anything is
wedged.** Brings the bridge up, `PING`s until it answers, reads
`GET_DIAG_INFO`'s `uptime_ms` (alongside `fw_version` and `reset_cause`,
both printed for reference), issues a host hard reset **alone** (no wedge
has happened yet), `PING`s again, and reads all three fields again. Prints
the `uptime_ms` values and states plainly whether the second is far enough
below the first to prove a real reboot — the single most important line
this app prints. If a healthy-session host reset does not drive `uptime_ms`
back down, the host's reset never reaches the chip, and every later "reset
did not revive it" observation — in this app and in the bench run that
motivated it — is meaningless. `reset_cause` is then cross-checked against
that `uptime_ms` verdict — it can only change via a genuine reset, so it is
independent corroboration — and when the two DISAGREE this app trusts
**neither**: the verdict becomes `UNDETERMINED` rather than a confident but
possibly wrong mechanism 2. Similarly, when `GET_DIAG_INFO` cannot be read
before and/or after the reset at all, the verdict is `UNDETERMINED`, never
a silent mechanism 2.

**PHASE B — wedge it.** Issues `WIFI_CONNECT_STA` with whatever credentials
are configured. The connect is **expected to fail** — that is the point,
not a bug to chase. This app never *widens* the connect timeout hunting for
a pass, but it does deliberately **shrink** it (2 s, not the ~55 s every
sibling app uses) — see `WEDGEPM_CONNECT_TIMEOUT_MS`'s own comment: the
goal is to stop `cc3501e_wifi_connect()`'s own 50 ms `WIFI_STATUS` poll
loop re-framing the link while the firmware works, not to observe the link
early.

A short connect budget alone is not enough, though: the firmware's own
connect body keeps holding the host off for up to 30 s of association plus
10 s of DHCP regardless of what this app's own timeout returns. So once
the connect call returns, PHASE B goes **silent** — no requests issued at
all — for `WEDGEPM_QUIET_WAIT_MS` (55 s, derived exactly like the sibling
apps' own 55 s connect budget: the firmware's documented 40 s worst case
plus the same 15 s reinitialisation margin), then issues **exactly one**
confirming `PING`. The short budget and the long silent wait are **not in
tension** — the budget stops this app's own driver code from re-framing
the link; the silent wait then lets that identical span of time pass with
the bus completely idle, so a healthy board gets the same window it would
have had regardless. If that single `PING` answers, the board was never
actually wedged this run: the app prints `=== WEDGE NOT REPRODUCED ===`
and **stops immediately**, before PHASE C or any recovery step runs —
there is nothing to diagnose, and running the recovery ladder against a
healthy board would print a confident but false "mechanism 1".

**PHASE C — post-mortem, in the wedged state, before any power cycle.**
Reached only when PHASE B's confirming `PING` did **not** answer, i.e. the
wedge actually reproduced this run. Captures, in this order:

- the receive scratch buffer after a `PING` request-header transceive —
  **only interpreted when `ping_rc` proves the driver's request path was
  actually entered** (see "Reading `rx_scratch`" below); classified
  against the three known repeated-byte patterns (`0xA5A5A5`, `0x000000`,
  `0xFFFFFF`), each printed with **every** wire-level origin known for
  that exact pattern (two or three, depending on the pattern) — never a
  single mechanism guessed from a tail alone;
- the `READY` pin level, read directly as a GPIO input and printed for
  completeness only — **this repo documents that exact pad as an open
  connection on the bench unit** (0 edges in 20000 samples during live
  traffic), so a read samples a floating input and carries **no
  evidentiary weight**; it never appears in the verdict;
- a host hard reset **alone**, then a `PING` — reports whether it
  answered. A revival here does **not** by itself prove a hung firmware
  task: bench precedent (`cc3501e_recover()`'s own doc comment) records
  wedges of one class clearing with a warm reset while the firmware
  stayed provably healthy throughout, so this app names both candidate
  origins whenever it reports a revival, and the verdict never asserts
  mechanism 1 from this alone;
- a full power-off, a hold of at least 2 seconds, then a power-on and
  reset, then a `PING` — reports whether it answered, **and whether the
  power cycle actually reached the wire** (a failed `cc3501e_reset()` makes
  every following `PING` short-circuit before clocking a single bit — see
  "Reading `rx_scratch`" below for the same short-circuit on the driver
  side). `ALP_ERR_VERSION` from that reset is reported as reaching the
  wire — the chip DID answer `GET_VERSION`, it is only refused for a
  protocol-major mismatch — never as "not attempted on the wire", which
  only genuine no-wire codes (including `ALP_ERR_BUSY`, a request-lock
  timeout before a single byte was clocked) still mean. Two seconds
  matters: `cc3501e_reset()`'s own built-in discharge gate is only 50 ms,
  and its own comment warns that a short gate risks a brown-out that
  skips the chip's chain-of-trust re-initialisation rather than a clean
  boot.

Finally prints **one verdict line** naming which of the three mechanisms
above the combined evidence points to — or that the verdict is
undetermined, when a required step (PHASE A's positive control, or PHASE
C's power cycle) never actually reached the chip, or when PHASE A's own
`reset_cause` and `uptime_ms` readings disagree with each other (see
"What it does" above — PHASE A now reports `fw_version` and `reset_cause`
alongside `uptime_ms`, and a disagreement between them is never silently
resolved in either direction).

## Reading `rx_scratch`

`cc3501e_t::rx_scratch` is a **public, documented** struct field (see its
doc comment in `<alp/chips/cc3501e/core.h>`), not a private internal this
app reaches into — the same field `aen-evk-demo`'s BLE_ENABLE failure probe
already reads for the identical reason. But two things stand between a raw
snapshot and real wire evidence, and `wedgepm_report_scratch()` handles both:

1. **Stale residue.** `cc3501e_ping()` calls straight through to
   `cc3501e_request()`, which short-circuits `ALP_ERR_NOT_READY` on a
   context that failed to (re-)initialise **without touching `rx_scratch`
   at all**. A snapshot taken after that short-circuit is leftover residue
   from an earlier call — this app checks `ping_rc == ALP_ERR_NOT_READY`
   first and reports it as stale, uninterpreted, rather than as fresh
   evidence.
2. **The poison byte, and its genuine ambiguity.** When the request path
   *was* entered, `cc3501e_request_locked()` (`chips/cc3501e/cc3501e_core.c`,
   the `out:` label) **unconditionally overwrites `rx_scratch[0]` to `0xDA`
   (`ALP_CC3501E_RX_SCRATCH_NO_STATUS`) on every pre-decode failure** —
   which a failed `PING` against a wedged link always is. That poison byte
   is exactly the ONE byte that would narrow down which of several
   wire-level origins produced bytes `[1..3]`. This app's classifier prints
   **every known origin** for whichever repeated-byte pattern the tail
   matches, and says plainly that a single snapshot cannot separate them,
   rather than naming one mechanism from a tail alone:
   - `0xA5A5A5` — three origins: the request-header phase's in-band armed
     check failing (a host- or slave-timing race), the slave's stale
     transmit FIFO echoing a previous phase's bytes (slave-side, not a host
     framing fault), or the reply-header phase's `hdr_ok` check failing
     consistent with (not proof of) the driver's "parked at a frame
     boundary" signature.
   - `0xFFFFFF` — two origins, **neither a hung firmware task**: the
     Puya-flash cold-boot bug (a transient boot-time condition, not a wedge
     in an already-running firmware), or a data-in mis-sampling fault on
     the Alif master's own MISO capture point (a host-side timing fault).
   - `0x000000` — two origins: the slave stuck mid-reply on a transfer it
     armed *earlier* and never completed (this app's own
     `src/cc3501e_bridge.h` documents this exact signature from the
     pre-existing `SOCK_RECV` desync defect — a stall left over from an
     unrelated earlier transfer), or the `#1378` dead-phase alias.

## Credentials — never committed

Follows the convention `aen-cc3501e-companion-tour` establishes: Wi-Fi
credentials arrive as **build-time defines**, with **empty defaults**,
under this app's own macro names (`WEDGEPM_*`, distinct from every other
`aen-cc3501e-*` sibling's own prefix, so a combined build never fights over
a shared `-D`). **No real SSID or passphrase is embedded anywhere in this
repository, including this file.**

```
west build ... -- -DEXTRA_CFLAGS="\
  -DWEDGEPM_WIFI_SSID=\\\"myssid\\\" \
  -DWEDGEPM_WIFI_PASS=\\\"mypass\\\" \
  -DWEDGEPM_WIFI_SECURITY=1"
```

`WEDGEPM_WIFI_SECURITY` selects the association type: `0` open, `1`
WPA2-PSK (the default), `2` WPA3-SAE.

When `WEDGEPM_WIFI_SSID` is empty (the build default), the app brings the
bridge up, confirms `PING`, runs PHASE A's positive control, then prints
that PHASE B and PHASE C are skipped and returns — there is nothing to
wedge the link with, so there is no post-mortem to take.

## Board target

`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` — the same target
`aen-cc3501e-command-sweep` and `aen-cc3501e-socket-throughput` use,
including their SRAM0 system-RAM placement and `CONFIG_DCACHE=n`. That
placement is load-bearing, not decorative: this app's own `PING` and
`READY` probes ride the exact SPI1 link every AEN801 CC3501E bench app
measures, and running this app faster (cache-on/DTCM) than every other
AEN801 bench app risks reproducing a different failure than the one this
app exists to characterise.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow), same shape
as its `aen-cc3501e-*` siblings:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-wedge-postmortem -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>;<fatfs>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cc3501e-wedge-postmortem/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

`<fatfs>` is only needed because `west list` in some worktree checkouts has
no `fatfs` entry even though the module is on disk — include it if your
build complains about a missing FatFs module; it is otherwise inert for
this app (it does not touch storage).

RAM-run over J-Link (no MRAM programming needed — the overlay retargets
`zephyr,flash` to ITCM). **The default build's primary read path is the
E1M edge UART0 console** (`CONFIG_UART_CONSOLE=y` in `prj.conf`) — connect
to it directly. Only for a serial-less bench: comment out the four
`CONFIG_SERIAL`/`CONFIG_CONSOLE`/`CONFIG_UART_CONSOLE`/
`CONFIG_UART_INTERRUPT_DRIVEN` lines and uncomment the three
`CONFIG_RAM_CONSOLE*`/`CONFIG_UART_CONSOLE=n` lines `prj.conf` already
carries, then read `ram_console_buf` over SWD instead — a default build has
**no** `ram_console_buf` symbol until you do that.

**Confirming a transcript is from THIS run, not a stale one:** the RAM
console buffer is **not cleared** between RAM-runs on this board, so a
stale transcript from a previous run reads exactly like a finished fresh
one. Before trusting a `ram_console_buf` read, power-cycle or re-flash and
re-run, and check the transcript opens with `=== AEN801 CC3501E wedge
post-mortem ===` and — critically — closes with either a `=== VERDICT ===`
block or an `=== WEDGE NOT REPRODUCED ===` line (both are legitimate,
complete endings — see "Expected duration" below); a transcript missing
both is either stale or truncated, not a finished fresh run.

**Expected duration:** varies a lot by outcome, and every number below is
an **ESTIMATE** derived from fixed constants, not a bench measurement.

- No credentials configured: PHASE A only, well under 15 seconds.
- Credentials configured, wedge **not** reproduced: PHASE A, plus PHASE B's
  `WEDGEPM_QUIET_WAIT_MS` silent wait (55 s) and its single confirming
  `PING`, then `=== WEDGE NOT REPRODUCED ===` and stop — roughly
  **65–80 seconds** total.
- Credentials configured, wedge **does** reproduce: the above, plus PHASE
  C's own recovery ladder — up to four more `cc3501e_hard_reset()` calls at
  ~3.5 s of blind boot settle each, the 2 s `WEDGEPM_POWER_OFF_HOLD_MS`
  hold, and, when neither recovery attempt revives the link, two exhausted
  5 s `PING`-retry loops — roughly **90–130 seconds** total through the
  `VERDICT` line.

**This estimate can stretch further, and unpredictably — do not use
elapsed time alone to judge a transcript.** `cc3501e_request()`'s own
reply gate polls the (floating, `PULL_NONE`, see PHASE C above) `READY`
pad before reading each reply phase; the first time that pad happens to
read HIGH, the gate settles into a 250 ms per-phase wait instead of its
usual short poll, and every later FAILED request in this run — which a
wedged link produces plenty of — can then cost roughly an extra second on
top of the numbers above. There is no fixed upper bound this app can quote
for that stretch. Recognise a complete transcript by its markers instead,
which are unaffected by how long the reply gate stretches: it opens with
`=== AEN801 CC3501E wedge post-mortem ===` and closes with either
`=== WEDGE NOT REPRODUCED ===` or a `=== VERDICT ===` block — a transcript
missing both closing markers is truncated, no matter how long it has been
running.

## What this app does NOT do

It never widens `cc3501e_wifi_connect()`'s timeout hunting for a pass,
never attempts recovery before PHASE C has captured every piece of
evidence, and never modifies `aen-evk-demo`, `aen-cc3501e-command-sweep`,
`aen-cc3501e-socket-throughput`, or `aen-cc3501e-companion-tour`. It never
runs PHASE C, or any recovery step, when PHASE B's own confirming `PING`
answers — see `=== WEDGE NOT REPRODUCED ===` above. It never asserts
mechanism 1 (a hung firmware task) from a warm-reset revival alone. It
never flashes anything and never touches the bench beyond a RAM-run —
build and run it against a board already under your own bench session. It
does not attempt to fix the wedge; it only characterises it.
