### Fixed — BMP581 configuration writes were silently discarded out of DEEP STANDBY (#2035)

`bmp581_set_sampling()` used to write `OSR_CONFIG` then `ODR_CONFIG`
straight out of the part's power-on state. BST-BMP581-DS004-13 §4.3 (p.16)
requires entering STANDBY first before any other mode transition, and
§4.3.8 (p.18) says writes to those two registers made outside STANDBY
"are lost" -- so the requested oversampling, ODR, and mode never actually
took effect on a freshly powered/reset part. `bmp581_set_sampling()` now
stages through STANDBY (with `deep_dis` set, so the part no longer falls
back into DEEP STANDBY on its own per §4.3.2) before writing either
config register, matching Bosch's own `bmp5_set_power_mode()`
(BMP5-Sensor-API) and upstream Zephyr's `bmp581` driver.

Covered by a fake-backed test asserting the actual register write order
on the wire, plus a standalone host harness (twister is unavailable in
this worktree) that mutation-verifies the fix.
