### Added — `aen-rtc-tick-probe` settles whether an intermittent RV-3028-C7 stall is the RTC, the read, or the host's own timebase (#2037)

A demo phase reads the RV-3028-C7, `k_msleep(1100)`, reads again, and requires
the seconds field to have changed. Across five cold runs of a byte-identical
image it failed once, with both reads returning `ALP_OK` and an unchanged
`00:00:10` — and the phase's own `slept_ms`, measured with `k_uptime_get()`,
the same clock that ran the sleep, read a clean 1100. The RV-3028-C7
Application Manual Rev. 1.4 documents no on-part mechanism for a successful
burst read of register `0x00` returning unchanged across a real gap of
>= 1000 ms with the oscillator running, which leaves the off-part
explanation: a kernel timebase running fast enough that 1100
kernel-milliseconds is under 1000 real milliseconds legitimately misses a
1 Hz tick, invisibly to every value the failing phase logged.

New standalone bench app on the E1M-AEN801, `examples/aen/aen-rtc-tick-probe`.
TEST A measures the kernel timebase directly against the RTC (+-5 ppm, the
better clock here) across a real 30 s window — the load-bearing number: a
delta other than 30 s +/- 1 means the kernel timebase itself is untrustworthy
on this SDK build, not just for this RTC phase. TEST B runs 55 x 1100 ms
intervals in a single boot (five samples is not a rate — the 95% interval on
a true 1-in-5 miss rate observed over five runs spans under 1% to over 70%)
and, on any interval where the seconds byte does not change, cross-checks two
independent tick witnesses: `STATUS` bit 4 (`UF`, set within one second of
the power-up default Second-update source per p.22) and the UNIX Time counter
at `0x1B`..`0x1E`, which the manual states "does not know such register
blocking" (p.52). Either witness showing a tick means the counter advanced
and the seconds byte itself read stale — the host or the I2C read, not the
part.

Never writes register `0x00` (Seconds) or `CONTROL_2` bit 0 (`RESET`) — both
reset the prescaler from 8192 Hz back to 1 Hz and restart the current second,
the one documented way to stretch a second past 1100 ms, which would
manufacture the very fault under investigation (p.14, p.24, p.85-86). The
only write this app performs on its own account is a plain `STATUS` write to
disarm `UF` between samples. Every register access is a single burst covering
`0x00`..`0x1E` per sample point, respecting the 950 ms register-blocking
window (p.52, p.53), with sample points kept >= 1100 ms apart. Raw bytes of
every read are printed in hex, always — decoded fields would hide exactly the
failure this app exists to catch.
