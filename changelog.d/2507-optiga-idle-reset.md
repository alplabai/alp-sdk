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
  means the part stayed silent even after a hardware reset. On V2N/V2M the hook
  wraps `gd32g553_se_reset()`; `optiga_trust_m_init()` is unchanged (no hook).
- New `tests/unit/optiga_trust_m_idle_reset` models the post-idle NACK pattern
  against the real driver and vendored library.
