### Fixed — MHUv2 `ipm_send()` returns -EBUSY while the previous word is unconsumed (#2983)

**`mhuv2_send()` no longer overwrites a pending `CH_SET` word.** It now checks `CH_ST` like `mhuv2_poll_out()` does, before asserting `ACCESS_REQUEST`, tests the whole 32-bit `CH_ST` word (not just bit 0, as `mhuv2_send_isr()` does) in `mhuv2_send()` and `mhuv2_poll_out()`, and returns `-EBUSY` without touching the request (it may be held high for the in-flight word), which is the `ipm_send()` contract. Covered by the new `tests/unit/ipm_mhuv2_send` register-model test.
