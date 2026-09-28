### Fixed — `alp_pwm_configure()` refuses undefined `break_cfg` bits instead of passing them through (#1648)

`break_cfg` only defines `ALP_PWM_BREAK_EXTERNAL`. The dispatcher forwarded
any other bit to the backend unchecked: the GD32 bridge wrote it straight
into its break field, and a caller got `ALP_OK` for a request nothing
honoured. The dispatcher now returns `ALP_ERR_INVAL` for a bit outside the
`ALP_PWM_BREAK_*` flags, the same way it treats an out-of-range
`align_mode`. The non-GD32 backends (Zephyr, Yocto, software fallback,
stub) already return `ALP_ERR_NOSUPPORT` for `alp_pwm_configure()`, so no
other `break_cfg` path accepts a request without effect.
