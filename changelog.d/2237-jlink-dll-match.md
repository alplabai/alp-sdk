### Fixed — bench J-Link runs use the install whose bundled firmware matches the probe (#2237)

`bench_jlink_run` used to launch the newest installed J-Link build. A
probe running older firmware was then protected from a bricking update
(the bench probes are OEM clones) only by the injected
`exec DisableAutoUpdateFW`. It now first reads the probe's running
firmware string over OpenOCD, by the labgrid-resolved USB path. OpenOCD
never offers an update. `bench_jlink_exe` then picks the newest install
whose bundled `Firmwares/JLink_V*.bin` carries exactly that string. That
is the same selection as the board-farm `jlink-run.sh`.

- It searches `ALP_JLINK_SEARCH_ROOT` (`JLink_Linux_V*_x86_64`) and
  SEGGER's default `/opt/SEGGER` (`JLink_V*`, overridable with
  `ALP_JLINK_OPT_ROOT`).
- It prints which install it chose and why.
- It falls back to the newest install when nothing matches or the
  firmware can't be read.
- `JLINK_EXE` still wins, and `DisableAutoUpdateFW` stays unconditional.

Bench: on E1M-AEN803 2026W36-0009 the probe reported
`J-Link V13 compiled Aug 26 2026 13:07:02`, and the V9.74 install was
chosen as the bundle match.
