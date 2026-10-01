### Fixed — provisioning tool power-cycled the PSU in ways that violate the RTL8211F(I) power rules (#2554)

Bench evidence showed a short ON blip (<1 s) between two OFF dwells of 3 s,
against the PHY datasheet (Rev 1.7, Table 53 notes 1-2: both 3.3 V and the PHY
1.0 V must reach 0 V, toggle period over 100 ms). `Power` now enforces a 10 s
minimum OFF dwell (a `bench.yaml` `power.off_s` below it is clamped up with a
warning) and a 5 s minimum ON time before any OFF. `bootstrap` reuses the live
SCIF ROM state `detect` just left instead of cycling again, and the
"Due to parameter error" ROM banner (DSW1 not in SCIF mode, or a faulty board)
is refused with an operator message instead of being treated as a download
prompt. `Power.cycle` keeps reading the console across the ON edge, and every
PSU command is logged with a monotonic timestamp together with the console
transcript in each step's log.

A fresh `Power` is treated as just switched on, so a new run started right after
another run's ON still waits the 5 s minimum. `boot_sd_linux` continues the boot
`dsw1_emmc_insert_sd` already started instead of cycling again. Every step's
log is written to the ledger on success too (long base64 push lines elided), and
a `gd32_flash` dry run plans the console or SSH transport with WOULD lines
instead of invoking the probe.

`boot_sd_linux` on a unit with no IP (blank GD32) now stays `done`: its probe
accepts the console login the run produced instead of demanding an IP, so
`gd32_flash` runs next over the console. A step flipped to failed by its
post-run probe still gets its step log written.

The console login now answers the shell profile's cursor-position query
(`ESC [ 6 n`) with a reply, waits for the console to go quiet, and exports
`TERM=dumb`. Interactive command lines are sent in 64-byte chunks with a short
gap, and each command's echo is checked against what was sent: on a mismatch
(a corrupted receive, spaces turned into `@` on silicon) the tool sends Ctrl-C
and resends once, then fails with a clear error instead of waiting 60 s.

After the console login the tool waits (up to 90 s) for `systemctl
is-system-running` to report running or degraded before issuing tool commands,
because the console corrupted single bytes while systemd was still starting.
`ConsoleTarget.run` re-sends a command whose begin marker never appears (Ctrl-C
first, up to three tries; markers are now plain `ALPB`/`ALPE`), and `put()`
writes each chunk to its own file, md5-checks it on arrival and re-sends only the
bad chunk. The discovered host is exported as `ALP_PROVISION_HOST` to the bench's
probe wrapper.
