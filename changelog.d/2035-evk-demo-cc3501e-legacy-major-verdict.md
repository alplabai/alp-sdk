### Fixed — aen-evk-demo failed the CC3501E phase against a healthy legacy-firmware link (#2035)

A bench session on an E1M-AEN801 unit running CC3501E firmware `3.1` saw
`examples/aen/aen-evk-demo` print `MAJOR MISMATCH` on `GET_VERSION` and fail
the whole phase, while every functional sub-check (`PING`, `GET_MAC` read
twice and agreeing, `GET_CAPABILITIES`, a four-network `WIFI_SCAN_START`,
`BLE_ENABLE`) passed. The host driver is deliberately bilingual during the
v3->v4 migration window (ADR 0033): it accepts a firmware `MAJOR` equal to
either `ALP_CC3501E_PROTOCOL_MAJOR` (4) or `ALP_CC3501E_PROTOCOL_MAJOR_LEGACY`
(3) -- `cc3501e_fw_major_is_acceptable()` in `chips/cc3501e/cc3501e_core.c`.
The demo's own verdict, `ver_ok = (ver_rc == ALP_OK) && (fw_major ==
ALP_CC3501E_PROTOCOL_MAJOR)`, never knew about the legacy branch and failed a
link the driver itself had accepted -- a legacy-major peer is the migration
working, not a mismatch.

Lifted the verdict out of `main.c`'s inline boolean into a small, testable
helper (`examples/aen/aen-evk-demo/src/cc3501e_link_verdict.h`,
`cc3501e_classify_link()`) that re-expresses the same two-constant rule
against the public `ALP_CC3501E_PROTOCOL_MAJOR[_LEGACY]` constants --
`cc3501e_fw_major_is_acceptable()` itself is declared in
`chips/cc3501e/cc3501e_internal.h`, `chips/`-internal and out of this demo's
reach. The phase now distinguishes and reports four outcomes instead of two:
exact major+minor match, major match with an additive minor delta (unchanged
wording), a legacy major with the host's bilingual acceptance named and
explained, and a genuine mismatch (`FAIL`, unchanged). Only the last of the
four still fails the phase.

Covered by five new native_sim ZTESTs
(`test_cc3501e_classify_link_*` in `tests/zephyr/chips/src/test_cc3501e.c`)
covering all four outcomes plus the `ver_rc != ALP_OK` case; verified by
mutation (removing the legacy branch reddens exactly
`test_cc3501e_classify_link_legacy_major_is_ok`, the scenario measured on
the bench, with the other 273 test cases in the suite unaffected).
