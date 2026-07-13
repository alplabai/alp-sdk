# mqtt-sn-publish

coreMQTT-SN teaching example. Encodes an MQTT-SN PUBLISH packet into a
buffer, then parses it back out of the same buffer and asserts the
topic id, message id, QoS, retain flag, and payload all survive the
round trip. No socket, no gateway, no live transport -- this is the
unit-test shape you'd run before wiring the encoder to a real UDP/
BLE/UART MQTT-SN link.

**[UNTESTED]** -- both in the "native_sim only, no bench" sense every
example in this batch carries, AND more specifically: the real
coreMQTT-SN library is not exercised at all here. See "Integration
reality" below.

## Integration reality

`board.yaml` declares `libraries: [coremqtt_sn]`, which is enough for
`scripts/alp_project.py` to emit `CONFIG_ALP_MQTTSN_NO_TLS=y` (the
SW-fallback default from
`metadata/library-profiles/coremqtt_sn/hw-backends.yaml`). It is
**not** enough to get the real library's headers onto the include
path, for two compounding reasons:

1. `west.yml` pins coreMQTT-SN as `freertos-org/coreMQTT-SN` under the
   `extras-tier1` group, which is disabled by default -- a customer
   must run `west update --group-filter +extras-tier1` at least once.
2. On this build machine, even that fetch failed: `modules/lib/
   coremqtt_sn/` exists but is an empty working tree (`git log` shows
   an orphan `init_placeholder` branch with zero commits pulled from
   the `freertos-org` remote). There is no `core_mqttsn_serializer.h`
   on disk anywhere to `#include`, and no way to build-verify the real
   library's call shape in this environment.

`src/main.c` therefore does **not** `#include` any coreMQTT-SN header
or call any of its functions. Instead it hand-rolls the identical
OASIS MQTT-SN v1.2 PUBLISH wire format (`mqttsn_wire_encode_publish()`
/ `mqttsn_wire_decode_publish()`) so the round trip still builds and
runs on `native_sim`, and produces the same bytes on the wire that
coreMQTT-SN's serializer would.

To swap in the real library once `west update --group-filter
+extras-tier1` succeeds: replace the two `mqttsn_wire_*()` calls in
`main()` with coreMQTT-SN's `MQTTSN_SerializePublish()` /
`MQTTSN_GetPublishPacketSize()` to encode and
`MQTTSN_DeserializePublish()` to decode, populating an
`MQTTSNPublishInfo_t` from the same `topic_id`/`msg_id`/`qos`/
`retain`/`payload` locals used here (coreMQTT-SN mirrors its sibling
coreMQTT library's `core_<lib>_serializer.h` / `<Lib>Status_t`
naming convention -- see
[github.com/FreeRTOS/coreMQTT-SN](https://github.com/FreeRTOS/coreMQTT-SN)).
Confirm the exact struct field names and function signatures against
the fetched `modules/lib/coremqtt_sn/source/include/*.h` headers --
they are **not** verified against a local checkout in this repo, only
against the module's public documentation.

## What this shows

* The OASIS MQTT-SN v1.2 PUBLISH wire format: 1-byte Length, 1-byte
  MsgType (`0x0C`), 1-byte Flags (DUP/QoS/Retain/Will/CleanSession/
  TopicIdType), 2-byte big-endian TopicId, 2-byte big-endian MsgId,
  then the raw payload.
* Publishing to a **predefined** topic id -- MQTT-SN's defining
  difference from full MQTT: a 2-byte id both ends already agree on,
  instead of a string topic name, to keep packets tiny on constrained
  links.
* A buffer-only encode/decode round trip with an explicit
  byte-for-byte assertion, the same pattern `mqtt-sn-publish`'s real
  coreMQTT-SN equivalent would use in a host unit test.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/connectivity/mqtt-sn-publish \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

```
[mqtt-sn-publish] encoding PUBLISH: topic_id=0x0001 msg_id=0x0007 "23.5C"
[mqtt-sn-publish] encoded 12 bytes
[mqtt-sn-publish] decoded: topic_id=0x0001 msg_id=0x0007 qos=0 retain=0 payload="23.5C"
[mqtt-sn-publish] round trip OK
[mqtt-sn-publish] done
```

## Hardware swap

This example has no board-specific wiring -- it's pure buffer logic.
Once wired to a real MQTT-SN gateway, the transport (UDP broadcast,
BLE GATT, or a UART framing layer) is what changes; the PUBLISH
encode/decode calls above stay the same regardless of SoM family.

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
- [OASIS MQTT-SN v1.2 specification](https://www.oasis-open.org/committees/documents.php?wg_abbrev=mqtt) --
  section 5.4.12 documents the PUBLISH message this example encodes.
- [github.com/FreeRTOS/coreMQTT-SN](https://github.com/FreeRTOS/coreMQTT-SN) --
  the pinned upstream library (`west.yml`, `extras-tier1` group).
