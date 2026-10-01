### Changed — AEN app-immutable update-log profile is a provisioned, selectable profile (#111)

The Alif E4/E8 dual-M55 `HW_ENFORCED` update-log tier is now selected by one
provisioning flag, `CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_APP_IMMUTABLE_PROFILE`,
instead of a hand-set `CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN`.
`..._FIREWALL_PROVEN` is now derived from the profile and no longer user-settable.
The profile also selects `CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED`, so an
image built for it fails `alp_update_log_open()` closed on a board whose firewall
was not provisioned.

Changes:
- `examples/connectivity/firmware-update-log` README gains a step-by-step
  deployment recipe (negative probe, profile build, flash, read-back). The OEM
  FC8 device-config step is documented as an out-of-SDK prerequisite.
- New native_sim scenario `alp.unit.update_log.require_hw` pins the fail-closed
  `REQUIRE_HW_ENFORCED` behaviour (owner absent or unproven, hard error, ready).
- `docs/os-support-matrix.md` and `include/alp/update_log.h` state the tier is
  app-immutable, not reflash-immutable.
