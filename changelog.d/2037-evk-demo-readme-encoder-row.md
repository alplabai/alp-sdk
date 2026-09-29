### Fixed - `aen-evk-demo`'s README still described phase 7 (rotary encoder) as an unimplemented stub (#2037)

`ab2a71dc3` implemented phase 7 as an attended, three-way `PASS`/`FAIL`/
`SKIPPED` verdict over the QEC0 quadrature pads and the UTIMER's own
`CNTR` register, but the README's phase table still read "Rotary encoder |
-- | Nothing (stub)." The row now describes the real hardware, the
attended-run flow, and the three-way verdict split.
