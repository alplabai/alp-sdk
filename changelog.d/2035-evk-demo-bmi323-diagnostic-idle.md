### Fixed — `aen-evk-demo`'s BMI323 init-failure diagnostic reproduced the exact timing bug it exists to diagnose (#2035)

`bmi323_diag_read16()` (`examples/aen/aen-evk-demo/src/main.c`) issued its
raw register reads back-to-back, with no interface idle between them --
exactly the access pattern the driver-side fix
(`changelog.d/2035-bmi323-communication-access-idle.md`,
`chips/bmi323/bmi323.c`) exists to avoid. BST-BMI323-DS000-13 Rev 1.7
section 7.3 "Communication Access Restriction" (p.205; `tIDLE,rd`,
Table 41 p.195) requires at least 450 us between consecutive accesses
while the device is in suspend mode, which every POR/soft-reset leaves it
in (section 5.4 p.20) -- exactly the state this diagnostic always runs in,
since it only fires after `bmi323_init()` itself has already failed. A
`CHIP_ID` read failing purely because it followed the previous access too
closely was misreported as `PHANTOM` (the part not answering the register
protocol at all) rather than the timing artefact it actually was.

`bmi323_diag_read16()` now waits the same 450 us suspend-mode idle after
its raw access, matching `chips/bmi323/bmi323.c`'s own `access_idle()`
rather than inventing a second convention.
