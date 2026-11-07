### Fixed — tests: swd_packet fuzz harness drives the production SWD driver (#2976)

The swd_packet fuzz harness checked an inline copy of the header and parity
logic, so a bug in chips/gd32_swd/gd32_swd.c could never fail it. It now
links the real driver and feeds a fuzz-controlled SWDIO bit stream through
gd32_swd_connect, halt, flash_write, verify and reset_and_run, faking only
the platform pad seam and alp_gpio_*. No driver source changed.

The ble_adv_parser, iot_mqtt, optiga_apdu and tls_handshake harnesses keep
their inline parsers: the SDK ships no parser of its own for those surfaces
(BLE adv data is passed through to the application, MQTT framing is Zephyr's
mqtt_client or Mosquitto, OPTIGA APDU decoding is the vendor library, and
there is no SDK TLS record helper), so there is no production function to link.
