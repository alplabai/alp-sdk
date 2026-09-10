### Fixed — `openocd-ram-run.sh`: preflight transcript race, a wrong-board USB-path example swept out of every site that repeated it, and a swallowed OpenOCD transcript (#2032)

Three defects in `scripts/bench/aen/openocd-ram-run.sh`, each found by a bench
operator on real hardware today and each costing real time or producing
misleading evidence.

`PREFLIGHT_OUT` was a fixed shared path,
`/tmp/openocd-ram-run-preflight.out`. Two concurrent runs — routine on this
farm, which has multiple AEN E8 boards — clobbered each other's transcript,
so an operator reading a preflight result after an abort could be reading a
different board's evidence. It is now `mktemp`'d per run
(`${TMPDIR:-/tmp}/openocd-ram-run-preflight.XXXXXX`), the path is echoed to
the operator before the preflight runs, and the file is only removed on the
success path — a failing preflight `exit 4`s before the cleanup line, which
is exactly when the file needs to stay behind to be read.

The script's own top-of-file safety-gate comment named `3-4.4.3` as "the AEN
E8" USB path for `AEN_OPENOCD_USB_LOCATION`. Live labgrid shows `3-4.4.3` is
`e1m-aen-evk-03`'s probe; `e1m-aen-evk-01`, the board this script is used
with most, is at a different path. All three AEN E8 boards on this bench
answer the identical SWD DPIDR (`0x4c013477`), so a mis-pinned USB path
still "looks healthy" through the whole DPIDR safety gate — the USB path is
the only thing that actually selects the board, and a copy-pasted example
naming a different board than the one an operator is holding is a silently
wrong flash waiting to happen. The comment no longer hardcodes any single
board's path; it tells the operator to resolve their board's path fresh
from `labgrid-client -p <place> show`'s `swd` resource every time.

The same stale `3-4.4.3` example, and the same fix, were carried in three
more places the fix above did not reach: `scripts/bench/aen/bench-env.sh`'s
`AEN_OPENOCD_USB_LOCATION` comment (line 424) and its
`bench_require_openocd` abort message (line 461, the message an operator
actually sees and is most likely to copy), and
`scripts/bench/aen/README.md`'s environment-variable table (line 92). All
three now point at `labgrid-client -p <place> show` instead of a hardcoded
path, for the same DPIDR-collision reason.
`docs/gd32-bridge.md` went further and asserted outright that `3-4.4.3` *is*
"the AEN E8" (lines 229 and 259-260, in the GD32 recovery-flash probe-swap
procedure); both are corrected to resolve the AEN E8's path per-board from
labgrid, with `3-4.4.3` now labelled explicitly as `e1m-aen-evk-03`'s path
so it cannot be misread as a generic "AEN E8" constant or copied by an
operator holding a different board. `scripts/bench/aen/README.md`'s
`openocd-ram-run.sh` table row also no longer claims the script is
"UNEXERCISED ON HARDWARE" — it ran today on `e1m-aen-evk-01` and produced
the restored `command_print` evidence described below, including
`113960 bytes written at address 0x00000000` and all four register echoes.

The final OpenOCD invocation joined its whole command chain
(`adapter usb location ...; init; [targets/arp_examine;] halt; load_image
...; reg ...; resume; shutdown`) into one `-c` string. OpenOCD suppresses
its own `command_print` output — the `load_image` byte-count line and the
`reg` register echoes — when a whole chain rides a single `-c`, so a
transcript captured that way could not distinguish a successful load from a
silent one. Bench-reproduced by running the identical sequence with each
command on its own `-c` and getting the full evidence back, including
`210684 bytes written at address 0x00000000` and the
`msplim_s`/`msplim_ns`/`msp`/`pc` register echoes. The command chain is now
built as a bash array with one `-c <command>` pair per entry, in the exact
same order as before — in particular the `he`-only `targets alif.m55he`
then `alif.m55he arp_examine` pair still lands between `init` and `halt`,
which is load-bearing, and the DPIDR preflight's exit-4 gate and the
existing exit-2 env-setup gate are both untouched.
