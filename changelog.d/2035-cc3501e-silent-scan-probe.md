### Added — `aen-cc3501e-silent-scan-probe` tests whether a silent host bus lets a `WIFI_SCAN_START`-wedged CC3501E link recover on its own (#2035)

`WIFI_SCAN_START` wedges the CC3501E SPI bridge on this bench, and
root-cause analysis has narrowed the cause to two readings every
measurement so far is equally consistent with: the firmware's own
station-role-up quiesce cancelling an armed transfer while the host is
still clocking the bus (bench-documented as locking the core up on three
separate occasions, but working at boot with no host traffic to disrupt),
or the radio operation itself simply not returning within its bound
regardless of host traffic. The standard driver call cannot separate them
— it polls status every 50 ms for its whole budget, and that polling is
itself the traffic under suspicion.

This new bench app submits `WIFI_SCAN_START` exactly once through the raw
`cc3501e_request()` path — the same submit-once shape
`cc3501e_wifi_ap_start()` already uses for `WIFI_AP_START` — then issues
**nothing at all** for 25 seconds: no status poll, no ping, no diagnostic
read, and no driver housekeeping either, since none exists in the CC3501E
driver tree (`chips/cc3501e/` and its attached backends carry no
`k_timer`/`k_work` of any kind). Only after that silence does it issue a
single confirming `PING`, then run a normal scan through the ordinary
`cc3501e_wifi_scan()` wrapper — if the scan role latched during the
silence, that second scan performs no role-up and should return promptly
with records. It prints a plain reading: whether the link survived the
silence, and whether the second scan returned records (with each record's
decoded security name, signal strength, and channel).

If the initial submit does not get its expected busy-style acknowledgement
(an I/O error or a timeout instead), the submit frame itself never
completed and the app declares the run `VOID` before the silence even
starts, rather than let a silent bus after a failed submit be misread as
evidence either way. Needs no Wi-Fi credentials — a scan takes none, and
that is part of why it is the right probe.

**Measured on silicon, and it answered the question it was built for.** Run
against the image carrying the role-up quiesce bracket: the submit was
acknowledged, and after the 25 s of silence the confirming `PING` never
answered — the bus stayed dead through roughly 70 s in total. So a silent
host does not rescue that build, which rules out a race against host
traffic as the whole story. Run against the published v0.8.0 image, same
app, same host build, same core, same steps: the submit was acknowledged,
the `PING` answered on the first attempt after the silence, and the second
scan returned records.

| | with the role-up quiesce bracket | published v0.8.0 |
|---|---|---|
| `PING` after the 25 s silence | never answered | ok, first attempt |
| second scan | no records | `ALP_OK`, records returned |

Scanning is stable on the published image: five networks, identical set and
ordering across four cold-booted runs, per-network signal spread no worse
than 3 dB. Two cautions from those runs, both worth more than the count. A
**single** scan is not enough to conclude a network is absent — one run
returned four records and omitted a fifth that four later runs all reported
at a stable level. And the **channel** field is the least trustworthy part
of a record: it moved between cold-identical runs while the matching signal
strength moved 2 dB.

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement, and `CONFIG_DCACHE=n` as
`aen-cc3501e-command-sweep`, for the same SPI1 FIFO-refill timing reason).
See `examples/aen/aen-cc3501e-silent-scan-probe/README.md`.
