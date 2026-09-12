### Added — `aen-cc3501e-socket-throughput` measures CC3501E end-to-end socket throughput over a correctly sized window (#2035)

A previous sweep (`aen-cc3501e-command-sweep`'s Part 2) reported
`STREAM_WRITE` throughput as 104 KB/s at 64 bytes and 250 KB/s at 256
bytes — artifacts of `k_uptime_get()`'s millisecond granularity timing
sub-millisecond transfers, not real rates: at the link's real ~980 KB/s, a
256-byte transfer takes ~255 µs, which a 1 ms-tick clock rounds up to
1 ms, reproducing the reported 250 KB/s exactly. The repo's own
`hal/ti/cc3501e_hw_ti_sock.c` (the CC3501E bridge firmware) records
end-to-end HTTP over the same bridge at 8 KB = ~660 kB/s,
16 KB = ~730 kB/s, 64 KB = ~742 kB/s, converging toward the believed real
~980 KB/s figure — the shape a genuine rate has and a timer-floor artifact
does not.

This new bench app copies that firmware file's own fix: a **windowed**
measurement, timing the whole read loop once with `k_uptime_get()` and
dividing once at the end, over a `SOCKTP_BYTE_BUDGET` of at least 4 MiB
(≈4 s at ~980 KB/s, a ~0.02% timer error instead of the 4x error above).
It brings the bridge up, associates to Wi-Fi, opens a TCP socket, issues
an HTTP GET, and reads the response body — skipping the HTTP headers,
timing only body bytes, clock started on the first body byte — until the
transfer completes or the budget is reached, printing a running-rate
progress line every ~512 KiB. It reports total body bytes, total wire
bytes, elapsed ms, derived rate (B/s and KB/s), and the `cc3501e_sock_recv`
call count with mean bytes/call, so a slow link and a chunky host are
distinguishable.

`cc3501e_sock_recv()` requests exactly its own wrapper-internal ceiling
(`ALP_CC3501E_MAX_PAYLOAD - sizeof(alp_cc3501e_sock_recv_resp_t) - 1` =
4071 B as of protocol v5, computed from the public wire struct, not the
driver's private size constant) rather than a round number, so every call
moves the largest chunk the API actually allows.

Wi-Fi credentials and the throughput server target arrive as build-time
`SOCKTP_*` defines with empty/placeholder defaults, the same convention
`aen-cc3501e-companion-tour` establishes — no SSID, passphrase, or server
address is embedded anywhere in the app. Build-only bench app for
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` (same target, overlay
memory placement, and `CONFIG_DCACHE=n` as `aen-cc3501e-command-sweep`,
for the same reason: the SPI1 FIFO-refill timing this link is sensitive
to). See `examples/aen/aen-cc3501e-socket-throughput/README.md`.

### Added — `aen-cc3501e-socket-throughput` gains a radio-free bridge-only mode (#2035)

The socket path above needs a live Wi-Fi association and a server that
answers, and that association has not yet completed on the bench — leaving
the app unable to produce a socket figure at all. So: when no credentials
are configured (the same `SOCKTP_WIFI_SSID`-empty condition STEP 3 already
checks — no new build-time define), this app now sweeps the
host-to-CC3501E SPI link alone via `STREAM_WRITE`, no radio involved,
instead of just returning. A credential-free build measures the bridge; a
credentialed build still measures end-to-end, unchanged.

It sweeps 64, 128, 256, and 512 bytes, ascending, each over its own 1 MiB
window (well above the 128 KiB floor the socket-path windowing above
derives; the same `k_uptime_get()` millisecond-granularity artifact that
produced the false 104/250 KB/s figures applies equally to `STREAM_WRITE`
timed per-call) and reports each size the instant it completes, so a wedge
partway through still leaves every already-completed size's real numbers
on the console. **The sweep never goes past 512 bytes**: 1024 B and 4092 B
`STREAM_WRITE` calls are measured, reproducible 3 of 3 on bench to fail
with `rc=-5` and leave the link permanently wedged for the rest of that
run; 512 B has since been measured clean 4 of 4 on bench, but the sweep
still stops immediately — printing which size failed, and not reporting it — on
the first failure rather than continuing into a dead link. Per size it
prints total bytes, elapsed ms, derived rate, call count, and mean
bytes/call, then states whether the rate curve flattens as size grows.

**This bridge-only figure is not comparable to an end-to-end HTTP figure.**
The bridge firmware's own `hal/ti/cc3501e_hw_ti_sock.c` records the
end-to-end number directly — roughly 660 to 742 kB/s (8 KB/16 KB/64 KB
transfers) — which includes the radio and everything downstream of it; the
bridge-only sweep measures one smaller segment of the path. See
`examples/aen/aen-cc3501e-socket-throughput/README.md`.
