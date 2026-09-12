### Added — `aen-cc3501e-wedge-postmortem` reproduces the CC3501E `WIFI_CONNECT_STA` wedge and captures forensic evidence while it is still down (#2035)

Issuing `WIFI_CONNECT_STA` over the CC3501E SPI bridge wedges the link: a
`PING` one second earlier answers on the first attempt and the connect
itself gets no reply at all, reproduced three of three on bench.

An earlier version of this text added "and the link then stays dead through
a host-driven reset pulse — only a full carrier power cycle revives it".
**That was wrong and is retracted.** It came from a run where a fresh Flow C
image was loaded over an already-running app, which double-faults the host
core into lockup before a single console byte (`clearing lockup after double
fault`, `pc: 0xeffffffe`) — the host core was dead, not the link. A later
run from a cleared buffer had a warm `nRESET` pulse revive the link on the
first attempt. Cold-cycle between Flow C runs, or this trap reads exactly
like a dead bridge.

Three mechanisms were still live: a hung firmware task
(chip otherwise alive), a host reset sequence that never actually reaches
the chip (which would make every "reset did not revive it" observation
meaningless), or a fault outside CC3501E firmware state entirely (Alif
SPI1/DMA, or an always-on power domain `WIFI_EN` cannot gate).

This new bench app separates them in a fixed, load-bearing order. PHASE A
is a positive control taken in a healthy session, **before** anything is
wedged: it reads `GET_DIAG_INFO`'s `uptime_ms`, `fw_version`, and
`reset_cause`, plus `DIAG_GET_STATS`'s `frames_ok`, brackets a host hard
reset with the host's own `k_uptime_get()`, and reads all four again —
proving a real reboot by comparing the second `uptime_ms` reading against
uptime-before-plus-host-measured-elapsed (a margin of ~3.5 s), not against
uptime-before alone (which a single failed PING retry can flip to false in
a healthy session), then cross-checking that verdict against `frames_ok`
(the firmware's own answered-frame counter, which collapses to a handful
across a genuine reboot and otherwise only climbs) and calling the whole
control `UNDETERMINED` — never a silent "mechanism 2 confirmed" — whenever
either reading is missing or the two signals disagree. `reset_cause` is
still printed for the transcript but is not the cross-check: this
firmware's own reset-cause handler folds a pin reset and a power-on reset
into the same value, so it reads unchanged across every host reset
regardless of whether the reset reached the chip, and cannot discriminate.
This is the one line every later "reset did/did not revive it" claim
depends on. PHASE B then issues the wedging `WIFI_CONNECT_STA` (expected
to fail) with build-time `WEDGEPM_*` credential defines (empty default, no
SSID/passphrase committed or ever printed to the console, same convention
as every `aen-cc3501e-*` sibling but its own macro prefix so a combined
build cannot collide) and a deliberately short 2 s connect budget — unlike
every sibling's ~55 s, chosen to stop the connect's own poll loop
re-framing the link while the firmware works. That short budget alone is
not enough, so PHASE B then waits **silently** (issuing nothing on the
bus) for the firmware's own worst-case association+DHCP window plus a
reinitialisation margin (55 s, derived the same way as the siblings' own
connect budget), and issues exactly one confirming `PING`: if it answers,
the app prints `=== WEDGE NOT REPRODUCED ===` and stops before any
recovery step runs, rather than characterising a healthy board as a hung
firmware task. PHASE C is the payload, reached only once that confirming
`PING` failed — but its own first `PING` is a *second*, independent
confirmation before anything else runs: PHASE B's single-shot ping can
fail spuriously on a healthy board, so a success here also prints
`=== WEDGE NOT REPRODUCED ===` and stops, rather than diagnosing a board
that was never wedged. Only once both pings have failed does it capture,
entirely in the wedged state before any recovery: the receive scratch
buffer after that `PING` — only interpreted when its return code proves
the driver's request path was actually entered, and, for the driver's
`0xDA` pre-decode poison byte (`rx_scratch[0]` on every failed request),
reported against every wire-level origin **this repository documents**
for whichever repeated-byte tail pattern it left (two or three per
pattern, including an added undriven-line origin for an all-`0xFF` tail)
or, when the surviving length field decodes as a plausible reply-header
length, the one pattern documented elsewhere in this repo as actually
meaning the link is wedged — a stale reply header, the slave answering
from one transfer behind — rather than naming one mechanism from a tail
alone, and without claiming the list is exhaustive — the `READY` pin level
(read directly as a GPIO input but labelled as carrying no evidentiary
weight, since this repo documents that exact pad as an open connection on
the bench unit), a host hard reset alone plus `PING` (a revival here is
reported against BOTH of its documented origins — a cleared hung task, or
the bench-proven case where a warm reset only re-syncs the host's own SPI
framing state while the firmware stayed healthy throughout — never
asserted as a hung task from the revival alone), then a full power-off
held for at least 2 seconds (`cc3501e_reset()`'s own built-in discharge
gate is only 50 ms, and its own comment warns a short gate risks a
brown-out that skips chain-of-trust re-init) followed by power-on + reset
plus `PING` — with the power cycle's own return codes checked so a failed
reset cannot masquerade as a wire-proven "did not revive", and
`ALP_ERR_VERSION` (the chip DID answer `GET_VERSION`, only its protocol
major was refused) reported as having reached the wire rather than as a
no-wire failure. A single failed `PING` after a reset that DID reach the
wire gets up to three extra hard-reset attempts — the same healthy-path
allowance `aen-cc3501e-gpio` gives its own cold-boot bring-up — before the
app concludes anything from it, since a single cold boot failing is a
known-benign outcome on this part. It prints one verdict line naming which
of the three mechanisms the combined evidence points to, or that the
verdict is undetermined when a required step never actually reached the
chip or PHASE A's own two signals disagreed.

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement, and `CONFIG_DCACHE=n` as
`aen-cc3501e-command-sweep`, for the same SPI1 FIFO-refill timing reason).
See `examples/aen/aen-cc3501e-wedge-postmortem/README.md`.
