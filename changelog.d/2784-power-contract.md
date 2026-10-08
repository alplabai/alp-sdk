### Added — power: STOP/STANDBY contract, per-mode wake check, SoM power-domain API (Refs #2784)

`alp/power.h` now documents that STOP and STANDBY may not return (wake is a cold
boot on the Alif Ensemble family) and that the 4 KB Utility SRAM is always
retained and SDK-reserved, so `ALP_POWER_RETAIN_NONE` means "no application RAM"
and the retention floor is the Utility-SRAM-retained rung; the `wake_after_ms`
wording now covers a timed wake bounded by a 32-bit low-power timer.

`alp_power_request_sleep()` returns `ALP_ERR_NOSUPPORT`, before the backend runs,
for a configured wake bitmap the requested mode cannot arm (new optional backend
op `mode_wake_caps`; backends without it keep today's behaviour).

New `[ABI-EXPERIMENTAL]` contract, `ALP_ERR_NOSUPPORT` on every backend for now:
`alp_power_domain_t`, `alp_power_domain_policy_t`, `alp_power_domain_info_t`,
`alp_power_boot_info_t`, `alp_power_domain_policy_set()`, `alp_power_domain_info()`,
`alp_power_boot_wake_info()`, and the `ALP_POWER_DOMAIN_BIT`, `ALP_POWER_ACTION_*`
and `ALP_POWER_DEP_*` macros.
