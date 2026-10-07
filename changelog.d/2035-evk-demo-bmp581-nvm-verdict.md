### Fixed — aen-evk-demo called a healthy, resting BMP581 BROKEN (#2035)

A bench session concluded the BMP581 on an E1M-AEN803 unit was `BROKEN`.
It never was: `STATUS` (0x28) reading `0x02` is that register's documented
power-on/soft-reset value (BST-BMP581-DS004-13 Rev 1.13 §7.22 p.58), and an
untouched BMP581 sits in DEEP STANDBY by design (§4.3 p.16, §4.3.2 p.16).
Bosch's own `bmp5_init()` and upstream Zephyr's `bmp581` driver both call
the part ready on exactly two bits -- `nvm_rdy` set, `nvm_err` clear --
which `0x02` already satisfies.

Three related defects in `examples/aen/aen-evk-demo`:

- The data-ready poll never enabled its own source. `INT_SOURCE` (0x15)
  resets to `0x00`, which §7.6 (p.54) documents as disabling interrupts
  other than power-on/soft-reset completion, so `INT_STATUS.drdy_data_reg`
  had nothing to assert. Now calls `bmp581_set_int_sources()` with
  data-ready enabled before polling, matching Bosch's own
  `read_sensor_data_forced_mode` example -- flagged in a comment as
  following Bosch's practice, not a settled datasheet guarantee.
- The health verdict required `INT_STATUS.drdy_data_reg` to assert, so the
  first defect's timeout alone was enough to report a working part as
  `BROKEN`. The verdict now matches Bosch/Zephyr's own criterion --
  `nvm_rdy` set and `nvm_err` clear (`bmp581_status_is_healthy()`, new
  `examples/aen/aen-evk-demo/src/bmp581_verdict.h`) -- alongside the
  existing bus-op and reset-sentinel checks, so a genuine fault (`nvm_err`
  set, a bus failure, an unreadable `CHIP_ID`) is still reported.
- A diagnostic printed `core_rdy` decoded from the same register with
  nothing branching on it, and no context distinguishing it from the two
  bits that actually matter -- which is what led a reader to call the part
  broken in the first place. The log line now names `nvm_rdy`/`nvm_err` as
  what the verdict is decided on and labels `core_rdy` explicitly as not a
  readiness signal (defined once in the datasheet, nothing waits on it, 0
  is its documented reset value).

Covered by a new native_sim ZTEST
(`test_bmp581_status_is_healthy_matches_bosch_nvm_criterion` in
`tests/zephyr/chips/src/test_sensors.c`) asserting `STATUS = 0x02` is
judged healthy and `nvm_err` set is still judged a fault; verified by
mutation (inverting the `nvm_err` comparison reddens the `STATUS = 0x02`
assertion).
