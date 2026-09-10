### Fixed — `aen-bmi323-regcheck` reported `por_detected` as always zero (#2035)

`examples/aen/aen-bmi323-regcheck` prints `STATUS (0x02)` including a
`por_detected` decode as part of its evidence trail. Once `bmi323_init()`
started reading `STATUS` itself as part of Bosch's device-initialisation
status test (this issue), that bit was already clear-on-read consumed by
the time this app got its own turn to read `STATUS` -- the printed
`por_detected` silently went from meaningful to always 0, exactly the kind
of misleading output this app exists to prevent.

It now calls `bmi323_was_por_detected()` right after `bmi323_init()` to
report the value `init()` actually observed (that accessor has no
`initialised` gate, so it works on a failing `init()` too), and the
`STATUS` printk lines say plainly that their own `por_detected` bit reads 0
here because `init()` already consumed it. `bmi323_init()`'s split return
codes (`ALP_ERR_NOT_READY` for a rejected POR gate, `ALP_ERR_IO` for a
`CHIP_ID`/`ERR_REG.fatal_err` failure) are now decoded in the app's `RESULT
FAIL` line instead of a single generic message. The mandatory
register-read-order comment in `src/main.c` and `README.md` are updated to
match: the order is still required, but now for `drdy_acc`, not
`por_detected`.
