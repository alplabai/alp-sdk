### Fixed — `counter` gd32-bridge out-of-range instance id returned `NOSUPPORT`, unlike its adc/dac/pwm siblings (#1635)

`src/backends/counter/gd32_bridge.c:33` returned `ALP_ERR_NOSUPPORT` for a `counter_id` at or
beyond `GD32G553_BRIDGE_COUNTER_CHANNELS`, citing an alp-sdk#1242 rationale that a published
E1M-X connector identity this SoM does not serve is not a bad argument. The `adc`, `dac`, and
`pwm` gd32-bridge siblings (`src/backends/{adc,dac,pwm}/gd32_bridge.c`) all already return
`ALP_ERR_INVAL` for the identical out-of-range condition, so this was drift, not a deliberate
per-peripheral distinction — this SoM vendor's SDK should give one answer to "you asked for an
instance that does not exist" across every peripheral class, and reserve `NOSUPPORT` for "this
build cannot do that at all", a different question.

`gd32_bridge.c` now returns `ALP_ERR_INVAL` for an out-of-range `counter_id`, matching its
siblings. The stale `:21-32` rationale comment explaining the old `NOSUPPORT` behaviour has been
rewritten so it documents the current code instead of contradicting it. `docs/portability.md`
§4.5 and the `ALP_E1M_X_COUNTER0..3` guidance in `include/alp/e1m_x_pinout.h` — both of which
asserted the old "a published id never reports `ALP_ERR_INVAL`" invariant for counter — are
updated to match. `tests/zephyr/peripheral/src/main.c`'s
`test_v2n_supervisor_counter_high_id_rejected` (the alp-sdk#1242 regression test) and
`tests/zephyr/peripheral/src/counter.c`'s `test_counter_unserved_published_id_status` now assert
`ALP_ERR_INVAL` on the V2N-supervisor (gd32-bridge) build; the generic Zephyr backend path is
unaffected and still asserts `ALP_ERR_NOT_READY` for an id with no devicetree alias.

Note: `src/backends/qenc/gd32_bridge.c`, the fourth backend this fix's rationale names as a
sibling that "already returns INVAL", in fact performs **no** range check on `encoder_id` at all
— it is not INVAL, it is unvalidated. Left untouched here as out of scope for #1635; flagged for
separate follow-up.

`src/backends/storage/zephyr_littlefs.c:169`'s `erase_size = 1u` was reviewed against the same
#1635 sweep and confirmed as a deliberate difference, not drift: this backend only ever touches
its mount by path (`fs_open`/`fs_stat`), never obtains a `struct device` or `flash_area` to query,
and littlefs's own logical block is a wear-levelling unit the filesystem chooses, not the
underlying physical erase geometry — unlike `src/backends/storage/zephyr_flash.c`, which derives
a real page size via `flash_get_page_info_by_offs()` because it holds a `flash_area` handle. A
comment at that line now says so explicitly, so a future reader does not "fix" it to match
`zephyr_flash.c`.
