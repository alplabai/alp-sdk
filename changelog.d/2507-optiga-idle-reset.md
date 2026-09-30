### Fixed — OPTIGA Trust M probe recovers a part that went silent after idle (alplabai/alp-sdk#2507)

On E1M-V2M103 the Trust M answers `I2C_STATE` (write `0x82`, read 4) while it is
accessed about once a second and right after an SE reset, but after more than
about 10 s idle it NACKs every register-address write, however long the host
polls, and only an SE reset brings it back. `optiga_trust_m_init()` then
reported `ALP_ERR_NOT_READY` for a fitted part.

Changes:
- New `optiga_trust_m_init_with_reset()` takes an optional RESET hook. When the
  probe spends the host library's NACK-polling budget (`PL_POLLING_MAX_CNT` tries),
  the driver pulses RESET once (low for the library's `RESET_LOW_TIME_MSEC`,
  then waits its `STARTUP_TIME_MSEC`) and probes again. `ALP_ERR_NOT_READY` now
  means the part stayed silent even after a hardware reset. `optiga_trust_m_init()` is unchanged (no hook). The hook is a parameter, not a
  context field, so `optiga_trust_m_t` keeps its layout.
- New `gd32g553_se_reset_hook()` adapts `gd32g553_se_reset()` to that hook type;
  `v2n-secure-element-sign` and `v2n-brd-i2c-bringup` now use it.
- Not covered: `read_product_info`/`send_apdu` on a context that idles after
  init still have no reset-and-retry (follow-up).
- New `tests/unit/optiga_trust_m_idle_reset` models the post-idle NACK pattern
  against the real driver and vendored library.

Status: verified with a host mock only; the on-silicon bench check (plain init
returns `-2` after idle, `init_with_reset` returns 0 with one pulse) is pending.
The 2 ms RESET-low pulse may need lengthening if it does not revive the part.
