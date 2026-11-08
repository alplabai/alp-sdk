### Fixed — MHUv2 `ipm_send()` returns -EBUSY while the previous word is unconsumed (#2983)

**`mhuv2_send()` and `mhuv2_poll_out()` no longer overwrite a pending `CH_SET` word.** Both now check the whole 32-bit `CH_ST` word (as `mhuv2_send_isr()` does, not just bit 0) before asserting `ACCESS_REQUEST`, and return `-EBUSY` without touching the request, which may be held high for the in-flight word; that is the `ipm_send()` contract. Covered by the new `tests/unit/ipm_mhuv2_send` register-model test.
