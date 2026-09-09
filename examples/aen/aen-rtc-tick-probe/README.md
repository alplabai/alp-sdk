# aen-rtc-tick-probe

Diagnostic bench app for the E1M-AEN801 (Alif Ensemble E8). Settles whether an
intermittent RV-3028-C7 (`chips/rv3028c7/rv3028c7.c`) seconds-field stall
(#2037) is the RTC, the I2C read, or the host's own kernel timebase.

## The failure

A demo phase reads the RV-3028-C7 time, `k_msleep(1100)`, reads again, and
requires the seconds field to have changed. Across five cold runs of a
byte-identical image it failed once: both reads returned `ALP_OK`, `t0 == t1
== 00:00:10`, and the phase's own `slept_ms` -- measured with
`k_uptime_get()`, the *same* clock that ran the sleep -- read 1100.

The RV-3028-C7 Application Manual Rev. 1.4 documents no mechanism by which a
successful burst read of register `0x00` returns unchanged across a real gap
of >= 1000 ms with the oscillator already running. Every on-part explanation
has been checked against the manual and ruled out, which leaves the
off-part one: if `slept_ms` is measured with the same clock that ran the
sleep, it cannot detect a fast kernel timebase. A `k_uptime_get()`-measured
1100 ms sleep that is actually under 1000 ms of real time legitimately
misses a 1 Hz tick -- and every value the demo logged would still read as a
clean success.

## What each test discriminates

### TEST A -- host timebase vs the RTC

Reads the RTC's UNIX Time counter (`0x1B`..`0x1E`), `k_msleep(30000)`, reads
again. The RV-3028-C7 is spec'd at +-5 ppm, so it is the better clock in this
experiment -- the kernel is the suspect. Also captures `k_uptime_get()` and
`k_cycle_get_32()` across the same window, and prints
`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`, so all three clocks can be compared
directly.

**This is the single most important number in this app.** PASS requires the
RTC-measured delta to read 30 s +/- 1. Anything else means the kernel
timebase on this SDK build is off -- not just for this RTC phase, but for
every `k_msleep`/`k_uptime_get()` call in the image -- and it invalidates the
"perfect part" premise every other candidate (including TEST B's own
1100 ms cadence) depends on.

### TEST B -- did the counter tick, even when the seconds byte did not change?

Runs the demo's own `1100 ms` cadence for >= 50 intervals in a single boot
(five samples is not a rate: the 95% interval on a true 1-in-5 miss rate
observed over five runs spans under 1% to over 70%). Each interval:

1. Burst-reads `0x00`..`0x1E` (time, `STATUS`, `CONTROL_1`/`CONTROL_2`, UNIX
   Time) in one I2C transaction.
2. Disarms `STATUS` bit 4 (`UF`, the periodic-update flag) with a plain
   register write -- the *only* write this app performs on its own account.
3. Sleeps 1100 ms.
4. Burst-reads again and compares.

For each interval where the decoded seconds byte did **not** change, two
independent witnesses are cross-checked:

- **`UF` (STATUS bit 4).** The manual states `UF` is set within one second of
  the Second-update source being selected -- the power-up default (p.22). If
  `UF` reads set on the "after" read despite unchanged seconds, the counter
  ticked and the *seconds byte itself* read stale -- points at the host or
  the I2C read path, not the part.
- **UNIX Time counter (`0x1B`..`0x1E`).** The manual states this counter
  "does not know such register blocking" (p.52) -- a separate, wider witness
  that a tick happened even if `UF` alone were ambiguous.

If **neither** witness shows a tick, the stall is real: the RTC's second
genuinely did not advance in that window.

Raw bytes of every burst read are printed in hex, always -- decoded fields
would hide exactly the failure this app exists to catch (a byte that reads
back identical when it should not have).

## Reading the output

```
=== TEST A: host timebase vs RV-3028-C7 over a real 30000 ms window ===
RTC unix-time delta:    30 s  (before=... after=...)
k_uptime_get() delta:   30000 ms
k_cycle_get_32() delta: ... cycles = ... ms (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=...)
TEST A RESULT: PASS -- RTC measured 30 s across the 30 s window; the kernel timebase is trustworthy here.
```

`TEST A RESULT: FAIL` with an RTC delta other than 30 means: trust the RTC
number, not the kernel's. Re-derive every other timing in this SDK build
from the RTC's clock, or from `k_cycle_get_32()` against a known-good
`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`, until the kernel timebase mismatch is
understood.

```
  [12] seconds 41 == 41 : STALL (UF=1 unix_delta=1) -- counter DID tick -- stale read (host/bus), not a real stall
  [13] seconds 44 == 44 : STALL (UF=0 unix_delta=0) -- counter did NOT tick -- genuine stall
...
TEST B SUMMARY: intervals=55 advanced=52 not_advanced=3 read_failures=0
  of the 3 non-advancing intervals: UF_set=2 UF_clear=1 | unix_ticked=2 unix_flat=1
```

`not_advanced` with `UF_set`/`unix_ticked` counts near zero (relative to
`not_advanced`) means the counter really is stalling -- a genuine on-part or
electrical finding. `UF_set`/`unix_ticked` counts close to `not_advanced`
means the seconds *read* is the unreliable part, not the counter -- consistent
with TEST A having already shown a kernel-timebase mismatch.

## Hard constraints (see the file header comment for citations)

- **Never write register `0x00` (Seconds), and never write `1` to
  `CONTROL_2` bit 0 (`RESET`).** Either resets the prescaler from 8192 Hz
  back to 1 Hz and restarts the current second, moving the next tick out by
  up to a full second -- the one documented way to stretch a second past
  1100 ms, which would manufacture the very fault under investigation. This
  app never does either; `rv3028c7_init()`'s own two writes (a `STATUS`
  `PORF`-clear and a `CONTROL_2` `12_24`-bit clear) are already
  silicon-verified safe by `examples/aen/aen-rtc-control2-probe`.
- **Respect the 950 ms register-blocking window.** One I2C transaction per
  sample point, covering everything needed (`0x00`..`0x1E` is one
  contiguous register block); sample points stay >= 1100 ms apart.

## Build

Standalone Zephyr app (no `alp_project.py` `board.yaml` flow), same target as
`aen-rtc-control2-probe`:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-rtc-tick-probe -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-rtc-tick-probe/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

RAM-run over J-Link (no MRAM programming needed -- the overlay retargets
`zephyr,flash` to ITCM); read `ram_console_buf` over SWD, or the E1M edge
UART0 console if the bench has one wired.

Run time: TEST A's 30 s window plus TEST B's 55 x 1100 ms intervals -- about
95 s total, plus I2C/printk overhead.
