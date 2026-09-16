### Fixed - `aen-evk-demo`'s I/O-expander phase asserted a configuration the SDK deliberately never creates (#2098)

`phase_io_expander()` (`examples/aen/aen-evk-demo/src/main.c`) read the
TCAL9538's configuration register (`0x03`) and printed it against a
hardcoded `0xF0` expectation. Nothing in the tree ever writes that register
for `P0-P3` -- deliberately, per #2035: they are carrier display/camera
control lines (`LCD_PWR_EN`/`LCD_RST`/`CAM_EN`/`CTP_RST`), not this SDK's to
own -- so a fresh board reads the power-on default `0xFF` every time, and
the polarity round-trip's own write (`tcal9538_set_polarity_inversion(&io,
0xFFu)`) inverted the register for all 8 pins, including the four the SDK
does not own.

Fixed: the config read is now printed as a found direction, per nibble
(`P0-3`/`P4-7`, `input`/`output`/`mixed`), never compared against an
expected value and never gated on. The polarity round-trip now only ever
writes `IOEXP_POLARITY_TEST_MASK` (`P4-P7`, the sensor-interrupt pins
`aen-sensor-int-probe` already legitimately drives). `PASS` requires `P4-P7`
to flip on invert (a fixed, nonzero compile-time mask, so this can't pass
vacuously the way #2037 found) AND the WHOLE byte to come back to `before`
on restore.

Two corrections from review, both against the same first draft of this fix:

- **The restore check was itself vacuous.** It compared only the bits this
  phase never writes (`P0-P3`), so a chip that ACKs the restore write but
  never actually clears register `0x02` still passed: `before=0xF3,
  inverted=0x03, restored=0x03` (invert genuinely took effect, restore
  never did) read as a `PASS` under the old mask. Comparing the whole byte
  catches it -- see `test_ioexp_polarity_vacuous_restore_now_fails` in the
  new `tests/zephyr/chips/src/test_ioexp_verdict.c`.
- **A strict single-attempt check can fail a healthy board.** `P4-P7` are
  live sensor-interrupt lines, and phase 2 leaves the ICM42670 running
  accelerometer output at 100 Hz for the rest of this app's run (it is
  never deinitialised) -- a real interrupt edge landing mid-round-trip can
  cancel one bit's forced flip. The predicate itself
  (`ioexp_polarity_check()`, new `examples/aen/aen-evk-demo/src/
  ioexp_verdict.h`) stays strict per attempt -- loosening it would also let
  a genuinely wedged expander pass -- so the phase instead retries the
  whole before/invert/restore sequence up to `IOEXP_ROUND_TRIP_ATTEMPTS`
  (3) times and accepts the first attempt that passes outright.

Both writes (`IOEXP_POLARITY_TEST_MASK` on invert, `0x00` on restore) are
still whole-register writes to `0x02` only (`tcal9538_set_polarity_
inversion()` has no read-modify-write) -- so `P0-P3`'s own polarity bits do
get written, always to 0, on every attempt. That is harmless today (`0x02`
sits at its POR default and nothing else in this tree ever writes it), and
is exactly what the now-whole-byte restore check would catch if it ever
stopped being true. What this phase never does, on any attempt or any
board revision, is touch the DIRECTION register (`0x03`) or the OUTPUT
register (`0x01`) for `P0-P3` -- it never drives or reconfigures those
pins, which is the ownership boundary #2035 actually cares about.

Does not reverse the #2035 decision.

Also extracts the AMP_FAULT (IRQ_N) raw-pin polarity mapping from #2097's
fix into `examples/aen/aen-evk-demo/src/amp_fault_verdict.h`, covered by
three new cases in `tests/zephyr/chips/src/test_audio.c` -- the same
predicate-in-a-header pattern as `ioexp_verdict.h`, so a re-inverted
ternary in either place now fails a unit test before it fails a bench run.
