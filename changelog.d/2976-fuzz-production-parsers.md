### Fixed — tests: fuzz harnesses now run the production parsers, not look-alikes (#2976)

The swd_packet, cc3501e and eeprom_manifest fuzz harnesses checked inline
copies of the code they were named after, so a bug in the shipped code could
never fail them. Each now links the real source and fakes only hardware seams:
swd_packet drives chips/gd32_swd/gd32_swd.c with a fuzz-fed SWDIO bit stream,
cc3501e drives cc3501e_reply_verdict() in chips/cc3501e/cc3501e_core.c, and
eeprom_manifest drives alp_hw_info_classify_manifest() in
src/zephyr/hw_info_zephyr.c, asserting the output is untouched on rejection.
No driver source changed.

The ble_adv_parser, iot_mqtt, optiga_apdu and tls_handshake harnesses are
deleted: the SDK ships no parser for those surfaces (BLE advertising data
belongs to the host stack, MQTT framing to Zephyr's mqtt_client or Mosquitto,
APDU decoding to the OPTIGA vendor library, and there is no SDK TLS record
helper), so they only gave false assurance. docs/threat-model.md and
tests/fuzz/README.md no longer claim that coverage.
