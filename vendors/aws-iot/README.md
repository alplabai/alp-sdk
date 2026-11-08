# vendors/aws-iot

Zephyr build glue for the [AWS IoT Device SDK for Embedded C][aws]
(MIT), pinned in `west.yml` at `202412.00` (project path
`modules/lib/aws-iot-device-sdk-embedded-C`, group `extras-cloud`, with
submodules).  No upstream source is copied here.

[aws]: https://github.com/aws/aws-iot-device-sdk-embedded-C

- **Enable:** `CONFIG_ALP_AWS_IOT=y` (needs `CONFIG_ALP_SDK=y`; defined in
  `zephyr/Kconfig.alp-libraries`, built via `zephyr/CMakeLists.txt`).
- **Fetch:** `west update --group-filter +extras-cloud`.
- **Builds:** coreMQTT (`core_mqtt.c`, `core_mqtt_state.c`,
  `core_mqtt_serializer.c`) and coreJSON (`core_json.c`), using upstream's own
  `mqttFilePaths.cmake` / `jsonFilePaths.cmake`.  Headers are on the global
  include path (`core_mqtt.h`, `core_json.h`, `transport_interface.h`).
  coreHTTP, device shadow, jobs, PKCS11, SigV4 are not built.
- **Config:** `MQTT_DO_NOT_USE_CUSTOM_CONFIG` is defined, so coreMQTT uses its
  `core_mqtt_config_defaults.h` rather than an application `core_mqtt_config.h`.
- **Checkout location:** `${ZEPHYR_BASE}/../modules/lib/aws-iot-device-sdk-embedded-C`,
  or the directory in `ALP_AWS_IOT_SDK_DIR` (CMake or environment variable).
  Upstream has no `zephyr/module.yml`, so Zephyr does not register it as a module.
- **Not provided:** the `TransportInterface_t` TLS shim.  The application
  supplies it (mbedtls is the natural provider).

Smoke test: `tests/zephyr/cloud_sdks/`.
