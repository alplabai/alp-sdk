### Fixed — MHUv2 `ipm_send()` returns -EBUSY while the previous word is unconsumed (#2983)

**`mhuv2_send()` no longer overwrites a pending `CH_SET` word.** It now checks `CH_ST` like `mhuv2_poll_out()` does, drops its access request, and returns `-EBUSY`, which is the `ipm_send()` contract. Covered by the new `tests/unit/ipm_mhuv2_send` register-model test.
