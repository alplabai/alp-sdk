### Added — `scripts/bench/aen/openocd-ram-run.sh`, an OpenOCD Flow-C RAM-run that can address the M55-HE core (#2037)

Bench-verified 2026-09-09: on `e1m-aen-evk-01`, CoreSight access port
`0x00200000` is the **M55-HP**, not the HE, and `0x00300000` is the HE — every
`ram-run.sh`/`flash-jlink*.sh` JLinkExe flow on this bench attaches to
whichever core the shared SW-DP exposes as "the live core", which turned out
to be the HP all along. That is how a kernel timebase running 2.5x fast (400
MHz measured, 160 MHz declared) went unnoticed for a whole campaign — see
`2037-aen-rtc-tick-probe-core-clock-identity.md`. No committed script knew the
HE existed.

New helper `scripts/bench/aen/openocd-ram-run.sh` drives OpenOCD against the
board-farm's shared, already-corrected SWD config (`AEN_OPENOCD_CFG`,
host-specific, no default — the config itself is out of this repo and
untouched). It runs the proven load sequence (`init`, optionally `targets
alif.m55he` + `alif.m55he arp_examine`, `halt`, `load_image <bin> 0x0 bin`,
clear `msplim_s`/`msplim_ns`, set `msp`/`pc` from the image's own vector
table, `resume`) against either core.

- **Default is HP, unchanged.** `core` defaults to `hp` and omits the two HE
  select lines, so a plain invocation reproduces exactly what every existing
  flow on this bench has always (if unknowingly) done. Pass `he` explicitly
  to opt in.
- **The core actually used is printed explicitly** (`core=M55-HE (AP
  0x00300000)` / `core=M55-HP (AP 0x00200000)`) before OpenOCD ever touches
  the probe — OpenOCD's own per-line target-name tag is not a reliable
  answer (it read `[alif.m55he]` for a whole campaign that was really
  driving the HP).
- **`core=he` prints a hazard warning, every run, not just the first**:
  `evk-01`'s HE ITCM carries a ~4.6 KB Secure-Enclave-resident stub
  (MSP `0x20040000`, reset vector `0x00000B58`) that `load_image` overwrites.
  It is RAM (power-cycle-recoverable, not a persistent MRAM write like Flow
  A/D), but this script does not restore or ask before clobbering it.
- Refuses (exit 1) on a slot0/MRAM-linked image (reset vector
  `>= 0x80000000`) — the same guard `ram-run.sh` already applies before its
  own `go`.

**UNEXERCISED ON HARDWARE.** Verified only by `shellcheck` (clean) and
`check_local_paths.py` (clean, verifying the config path stays an env var and
never a literal maintainer path). A real bench run should print the
`core=...` line before any OpenOCD output, then OpenOCD's own transcript
should show the target reaching `halt`ed state before `resume` — proof the
selected AP is live and the image is running on the intended core.
