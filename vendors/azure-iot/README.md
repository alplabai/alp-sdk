# vendors/azure-iot

Zephyr build glue for the [Azure SDK for Embedded C][az] (MIT), pinned in
`west.yml` at `1.5.0` (project path `modules/lib/azure-sdk-for-c`, group
`extras-cloud`).  No upstream source is copied here.

[az]: https://github.com/Azure/azure-sdk-for-c

- **Enable:** `CONFIG_ALP_AZURE_IOT=y` (needs `CONFIG_ALP_SDK=y`; defined in
  `zephyr/Kconfig.alp-libraries`, built via `zephyr/CMakeLists.txt`).
- **Fetch:** `west update --group-filter +extras-cloud`.
- **Builds:** az_core without the HTTP pipeline (`az_span`, `az_json_*`,
  `az_base64`, `az_context`, `az_log`, `az_precondition`), `az_noplatform.c`
  (upstream's no-op platform layer: no Zephyr clock/sleep port), and the az_iot
  common + hub client sources (telemetry, C2D, twin, methods, commands,
  properties, SAS).  The DPS client is not built.  Headers are on the global
  include path (`<azure/core/az_span.h>`, `<azure/iot/az_iot_hub_client.h>`).
- **Checkout location:** `${ZEPHYR_BASE}/../modules/lib/azure-sdk-for-c`, or the
  directory in `ALP_AZURE_IOT_SDK_DIR` (CMake or environment variable).
  Upstream has no `zephyr/module.yml`, so Zephyr does not register it as a module.
- **Not provided:** the MQTT/TLS transport; the application supplies it.

Smoke test: `tests/zephyr/cloud_sdks/`.
