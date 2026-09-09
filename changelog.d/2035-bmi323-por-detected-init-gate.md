### Fixed — `bmi323_init()` now requires STATUS.por_detected before trusting CHIP_ID (#2035)

`bmi323_init()` (`chips/bmi323/bmi323.c`) wrote the soft-reset command and
then went straight to a `CHIP_ID` read. That is not enough: `CHIP_ID`'s
reset value is `0x0043` (BST-BMI323-DS000-13 Rev 1.7, Table 36 p.61) and
reads back correctly whether or not the soft-reset write actually landed --
a part that ACKs the write without applying it, or that never left its own
prior POR state, passes the old `CHIP_ID` check and returns `ALP_OK` anyway.

`bmi323_init()` now performs Bosch's device-initialisation status test
(BST-BMI323-DS000-13 Rev 1.7, Figure 2 pp.15-16) before ever reading
`CHIP_ID`: it reads `ERR_REG` (`0x01`), then `STATUS` (`0x02`), and requires
`STATUS.por_detected` (bit 0, clear-on-read, set only by a real POR/soft-reset
event) to be set. If `por_detected` reads back 0, `bmi323_init()` now returns
`ALP_ERR_NOT_READY` instead of proceeding to a `CHIP_ID` check that would
have falsely passed. `ALP_ERR_IO` is reserved for a `CHIP_ID` read failure, a
`CHIP_ID` mismatch, and `ERR_REG.fatal_err`, so the four causes stay
distinguishable at the call site.

The bit is clear-on-read, and `bmi323_init()` consumes it. Callers that need
to know what init saw must use `bmi323_was_por_detected()`; re-reading `0x02`
afterwards always yields 0.

**This is a real behavioural change, not an internal tweak.** Silicon that
previously initialised successfully under the old, weaker check -- because
its `CHIP_ID` happened to match even though the reset never really applied
-- can now fail `bmi323_init()` where it did not before. That is the point
of the gate (it turns a silent false-`ALP_OK` into a diagnosable
`ALP_ERR_NOT_READY`), but it is a behaviour change any caller relying on the
old, looser pass criterion needs to know about.
