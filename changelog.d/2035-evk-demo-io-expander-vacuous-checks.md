### Fixed - `aen-evk-demo`'s I/O-expander round-trip could pass vacuously, and could fail on a real interrupt (#2035)

Two correctness gaps in `phase_io_expander()`
(`examples/aen/aen-evk-demo/src/main.c`):

- The polarity-inversion check was `((inverted ^ before) & cfg) == cfg`.
  When `cfg` (the TCAL9538's own configuration register, 0x03) reads
  `0x00` -- every pin configured as an output -- `(x & 0) == 0` is true
  for any `x`, so a wedged expander returning one constant byte on every
  read would have passed. The check now also requires `cfg != 0` before
  the mask comparison counts as evidence of anything, and the failure
  message says which condition wasn't met.
- The restore check was a flat `restored == before`. `P4`-`P7` on this
  expander are live sensor interrupt lines (ICM42670 `INT1`/`INT2`/
  `FSYNC`, BMP581 `INT1`), so a real interrupt edge landing between the
  two reads failed the phase for a reason that was never a fault. The
  comparison is now masked to the pins that cannot move on their own (the
  output-configured pins per the chip's own `cfg` read-back), and the
  excluded pins are named in a comment at the call site.
