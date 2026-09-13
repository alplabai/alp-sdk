### Fixed — `openocd-ram-run.sh` invoked OpenOCD with no USB path, no labgrid, and no DPIDR check (#2037)

**Safety fix, not a tidy-up.** `openocd-ram-run.sh` landed earlier today
(alp-sdk#2037) with its final line a bare `openocd -f "$CFG" -c "$CMDS"` —
no `adapter usb location`, no labgrid resolution, no SW-DP identity check —
even though the shared config it loads deliberately omits that USB location
and its own header says why: it is loaded by labgrid's `OpenOCDDriver`,
which supplies the location itself, and a by-hand run on the exporter must
prepend the flag rather than edit it in.

alplab-gw has **three J-Links, and two answer the same cloned OEM serial**
(`603000869`): the AEN E8 at USB path `3-4.4.3` and the GD32 bridge on a
different board (V2N-M1) at `3-4.2`. With nothing pinning the path, OpenOCD
picks a probe arbitrarily, and this script's first hardware actions are
`halt` followed by `load_image ... 0x0` — on the wrong probe, that halts and
overwrites a board the reservation does not cover. It was caught reading the
script, not on the bench: the bench runner drove OpenOCD by hand instead, so
nothing was damaged.

Two independent gates now sit in front of any hardware touch:

- **`AEN_OPENOCD_USB_LOCATION`** (new in `bench-env.sh`, no default,
  host-specific like `SE_UART`/`AEN_OPENOCD_CFG`) is required by
  `bench_require_openocd()` — unset, the script exits **2** before opening a
  build directory, resolving the vector table, or touching the probe.
  Resolve it from `labgrid-client -p e1m-aen-evk-01 show`'s `swd` resource;
  it is passed as `adapter usb location <value>`, prepended on the OpenOCD
  command line exactly as the shared config's header instructs.
- **A read-only `init; shutdown` preflight** runs before the real
  `init; halt; load_image` sequence and is checked with the same
  `bench_jlink_assert_aen_dpidr()` the JLinkExe flows already use (the AEN
  E8 answers `0x4C013477`; the GD32 bridge answers `0x0BE12477`, so the
  check genuinely discriminates the two boards sharing a serial on this
  bench). A mismatch aborts (exit **4**) naming which board actually
  answered — before `halt` or `load_image` ever runs.

The `core=hp` default, the `he` opt-in with its explicit AP print and
resident-HE-ITCM-stub warning, and the slot0/MRAM-linked-image refusal are
unchanged.

**UNEXERCISED ON HARDWARE.** Verified with `shellcheck -x` (clean),
`check_local_paths.py` and `check_cross_platform.py` (clean — the script
stays in `INTENTIONALLY_BASH_HELPERS`) only; the board is under an active
reservation. A real run should print, in order: `>>> openocd-ram-run
preflight (M55-HE / M55-HP)  usb=<AEN_OPENOCD_USB_LOCATION>`, then OpenOCD's
own line containing `0x4c013477` (case-insensitive match), then `>>>
openocd-ram-run <name>  core=... msp=... pc=...`, then OpenOCD's transcript
reaching `halt`ed state before `resume`. On the wrong probe, it should print
`bench_jlink_assert_aen_dpidr`'s `!! ABORT` naming the board that actually
answered and exit 4, with `halt`/`load_image` never appearing.
