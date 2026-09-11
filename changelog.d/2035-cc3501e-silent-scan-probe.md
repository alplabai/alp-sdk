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

Build-only bench app for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`
(same target, overlay memory placement, and `CONFIG_DCACHE=n` as
`aen-cc3501e-command-sweep`, for the same SPI1 FIFO-refill timing reason).
See `examples/aen/aen-cc3501e-silent-scan-probe/README.md`.
