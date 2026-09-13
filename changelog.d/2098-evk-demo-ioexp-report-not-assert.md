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
`aen-sensor-int-probe` already legitimately drives) -- `P0-P3` are never
named in either the invert or the restore write. `PASS` now requires
`P4-P7` to flip on invert (a fixed, nonzero compile-time mask, so this can't
pass vacuously the way #2037 found) AND `P0-P3` to stay byte-identical
across the whole round trip, since this phase never touches them in either
direction. The printed line now names which bits were exercised (`P4-P7`
polarity round-trip) and which were only observed (`P0-P3`).

Does not reverse the #2035 decision: `P0-P3` are still never configured,
driven, or included in the polarity write, on any board revision.
