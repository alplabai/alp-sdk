### Fixed - `aen-evk-demo`'s BMI323 init-failure diagnostic named the wrong cause on every failure but one (#2035)

`bmi323_report_init_failure()` (`examples/aen/aen-evk-demo/src/main.c`)
re-read `STATUS` (`0x02`) after `bmi323_init()` had already returned, and
printed "0 here is why init() failed: the soft-reset write never actually
landed" whenever that bit read clear. `STATUS.por_detected` is
clear-on-read (BST-BMI323-DS000-13 Rev 1.7 p.66) and `bmi323_init()`
already consumes it before returning, so a post-mortem re-read always saw
0 -- the blame text fired on every init() failure, including a `CHIP_ID`
mismatch or a `CHIP_ID` read error where the POR gate had already passed.

Now uses `bmi323_was_por_detected()` (added on top of #2035's driver-side
split, `chips/bmi323/bmi323.c`), which reports what `init()` itself saw
before that bit was consumed, together with `bmi323_init()`'s own split
return codes (`ALP_ERR_NOT_READY` for the POR gate, `ALP_ERR_IO` for
`CHIP_ID`/`ERR_REG.fatal_err`) to name which of the three actually
happened, instead of one story for all of them.
