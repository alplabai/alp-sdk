### Changed — AEN app-immutable update-log profile is a provisioned, selectable profile (#111)

The Alif E4/E8 dual-M55 `HW_ENFORCED` update-log tier is now selected by one
provisioning flag, `CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_APP_IMMUTABLE_PROFILE`,
instead of a hand-set `CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN`.
`..._FIREWALL_PROVEN` is now derived from the profile and no longer user-settable.
The profile also selects `CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED`, so an
image built for it fails `alp_update_log_open()` closed when the trusted owner is
absent. Firewall provisioning is asserted by the build, not checked at runtime;
only the negative probe verifies it.

Changes:
- `examples/connectivity/firmware-update-log` README gains a step-by-step
  deployment recipe (negative probe, profile build, flash, read-back). The OEM
  FC8 device-config step is documented as an out-of-SDK prerequisite.
- New native_sim scenario `alp.unit.update_log.require_hw` pins the fail-closed
  `REQUIRE_HW_ENFORCED` behaviour (owner absent, hard error, ready).
- `docs/os-support-matrix.md` and `include/alp/update_log.h` state the tier is
  app-immutable, not reflash-immutable.
- The deployment recipe now carries the FC8 DEVICE config in every ATOC package
  (a package without it de-provisions the firewall on SES v1.110).
  `flash-update-log-dual.sh` and `flash-update-log-firewall-probe.sh` take
  `ALP_AEN_DEVICE_CONFIG_JSON` (a path, which implies inclusion) and warn when it
  is absent; the example ships `fc8-dual-device-config.json`. The recipe also
  documents `ALP_ATOC_ALLOW_OVER_STORAGE`, the 64 KB deny window and the A32_APP
  overlap caution.
- The HE client now prints that its store is owned by the HP owner instead of
  "RAM fallback", and `read-update-log-proof.sh` uses a private `mktemp` dir.
