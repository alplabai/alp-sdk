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

The `mqtts://` path is configured through the portable API (broker URI + a
pinned CA) and **mbedTLS is now built in on every target**, native_sim
included. It used to be held off: with mbedTLS' PSA core disabled the pinned
library's own `ssl_misc.h` does not compile (`unknown type name
'mbedtls_error_pair_t'`), so the app turned mbedTLS off rather than fail. The
SDK now turns that PSA core on wherever it builds mbedTLS without TF-M
(`ALP_SDK_MBEDTLS_PSA_CRYPTO`, issue #2173), so this app carries no mbedTLS
knobs at all any more (see [`prj.conf`](../mqtt-telemetry/prj.conf), which is
empty by design) and the `native_sim.conf` that used to hold the workaround is
deleted.

The record stays `preview` for a different reason: on an AEN target the
random-number source is Zephyr's non-cryptographic fallback -- no Alif
Ensemble entropy driver exists yet, in this tree or upstream, and the boot log
says so (`WARNING: Using a test - not safe - entropy source`). The handshake
builds and runs; it is not production-grade key material until the SE TRNG is
wired to a real entropy driver (issue #2192).

## The "sensor reading"

To keep the focus on the transport, this template publishes a
**synthetic metric** (device uptime). Swap
`read_telemetry_value()` for a real sensor read -- e.g. compose it
with the [`sensor` template](../../peripheral-io/i2c-master/) (BMP581
over `<alp/chips/bmp581.h>`) -- and the publish path is unchanged.

## Build

```bash
# Standalone, native_sim (no radio, so the app prints the framing it
# would publish; mbedTLS is built in):
west build -b native_sim/native/64 examples/connectivity/mqtt-telemetry \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon (E1M-AEN801):
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/connectivity/mqtt-telemetry
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
