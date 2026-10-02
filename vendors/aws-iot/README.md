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
- **Transport:** coreMQTT has none of its own.  With
  `CONFIG_ALP_CLOUD_TRANSPORT=y` this directory also builds
  `alp_cloud_transport_coremqtt.c`: a `TransportInterface_t` over
  `<alp/cloud_transport.h>` (Zephyr TLS sockets + mbedtls, peer verification
  always on).  Include `"alp_cloud_transport_coremqtt.h"`; it defines
  coreMQTT's `struct NetworkContext`, so an application that brings its own
  transport must not include it.

  ```c
  alp_cloud_transport_t t = ALP_CLOUD_TRANSPORT_INIT;
  TransportInterface_t  iface;
  NetworkContext_t      net;

  alp_cloud_transport_connect(&t, &cfg);          /* <alp/cloud_transport.h> */
  alp_cloud_transport_coremqtt(&iface, &net, &t); /* then MQTT_Init(..., &iface, ...) */
  ```

  Set `recv_timeout_ms` to a short non-zero value: coreMQTT's process loop
  relies on `recv` returning 0 on a timeout and a negative value once the peer
  has closed.  AWS IoT Core on port 443 needs ALPN `"x-amzn-mqtt-ca"`
  (`CONFIG_MBEDTLS_SSL_ALPN=y`).

Smoke test: `tests/zephyr/cloud_sdks/`.  Transport tests:
`tests/zephyr/cloud_transport/` (the coreMQTT scenario needs this checkout and
runs in `nightly-cloud-sdks.yml`).  Not verified: a connection to a live AWS
IoT Core endpoint.
