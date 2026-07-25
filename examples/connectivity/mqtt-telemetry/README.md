# mqtt-telemetry

The **connected device** starting point: bring up Wi-Fi, open an
`mqtts://` (TLS) MQTT client, and publish a telemetry reading on a
cadence -- the whole path on the portable [`<alp/iot.h>`](../../../include/alp/iot.h)
surface.

```
alp_wifi_open() -> alp_wifi_connect()          # Wi-Fi station up
alp_mqtt_open(mqtts://... , CA pinned)          # TLS MQTT client
alp_mqtt_connect() -> alp_mqtt_publish() x N    # publish telemetry
alp_mqtt_close()                                # clean disconnect
```

## Supported hardware -- AEN only

This template ships for **E1M-AEN801 only**. Its transport is the
**CC3501E Wi-Fi6+BLE coprocessor bridge**
([`docs/cc3501e-bridge.md`](../../../docs/cc3501e-bridge.md)),
silicon-validated 2026-06-24. The app never names the bridge -- the
AEN board emit wires it as the `<alp/iot.h>` Wi-Fi backend from
`iot.wifi: true` in `board.yaml`.

The **E1M-V2N101 is deliberately not supported**: its Murata Wi-Fi
(`murata_lbee5hy2fy`) is `hil_silicon: untested` and its Linux-side
data path is an unmerged design, so there is no working Wi-Fi
transport for that family yet. Do not add it to a scaffold of this
template until that port lands.

## TLS status -- preview

The `mqtts://` path is configured through the portable API (broker
URI + a pinned CA), but **`CONFIG_MBEDTLS` is held OFF in the build
today**. Zephyr v4.4's mbedtls 3.6 has an `ssl_misc.h`
include-order bug (`unknown type name 'mbedtls_error_pair_t'`); full
`tf-psa-crypto` wiring is a v0.6 work item. On real AEN silicon the
build follows the TF-M stack and is unaffected; the native_sim leg
turns mbedtls off (see [`native_sim.conf`](native_sim.conf)) so the
framing path still builds and runs. This is why the catalog record
is `preview`.

## The "sensor reading"

To keep the focus on the transport, this template publishes a
**synthetic metric** (device uptime). Swap
`read_telemetry_value()` for a real sensor read -- e.g. compose it
with the [`sensor` template](../../peripheral-io/i2c-master/) (TMP112
over `<alp/chips/tmp112.h>`) -- and the publish path is unchanged.

## Build

```bash
# Standalone, native_sim (no radio; framing-only, mbedtls off):
west build -b native_sim/native/64 examples/connectivity/mqtt-telemetry \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd) -DEXTRA_CONF_FILE=native_sim.conf
west build -t run

# On real silicon (E1M-AEN801):
west build -b alp_e1m_aen801_m55_hp examples/connectivity/mqtt-telemetry
west flash
```

## Expected output

native_sim (no Wi-Fi/MQTT backend; framing only):

```
[mqtt] alp-sdk mqtt-telemetry demo (publish over MQTT/TLS)
[mqtt] transport: CC3501E Wi-Fi bridge (E1M-AEN801)
[mqtt] wifi: opening station
[mqtt] wifi: alp_wifi_open -> NULL (ALP_ERR_NOT_READY)
[mqtt] no transport on this build -- printing framing only
[mqtt] would publish to alp/telemetry/e1m-aen801: {"device":"e1m-aen801-demo","metric":"uptime_s","value":0}
[mqtt] would publish to alp/telemetry/e1m-aen801: {"device":"e1m-aen801-demo","metric":"uptime_s","value":5}
[mqtt] would publish to alp/telemetry/e1m-aen801: {"device":"e1m-aen801-demo","metric":"uptime_s","value":10}
[mqtt] done
```

Real hardware (E1M-AEN801, associated + broker reachable):

```
[mqtt] wifi: associated to "alp-demo-ap"
[mqtt] broker: connected
[mqtt] publish 0 -> ALP_OK: {"device":"e1m-aen801-demo","metric":"uptime_s","value":0}
...
[mqtt] done
```

## Reference

- [`<alp/iot.h>`](../../../include/alp/iot.h) -- Wi-Fi station + MQTT client surface.
- [`docs/cc3501e-bridge.md`](../../../docs/cc3501e-bridge.md) -- the AEN Wi-Fi transport.
- [`examples/peripheral-io/i2c-master/`](../../peripheral-io/i2c-master/) -- the `sensor` template, for a real reading to publish.
