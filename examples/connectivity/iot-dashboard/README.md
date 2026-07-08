# iot-dashboard

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Builds clean on
> `native_sim/native/64`; real WiFi + MQTT + TLS bring-up gates on
> v0.6 AEN HiL.

Customer-facing IoT demo: an E1M-AEN module reads BME280
environment samples (temperature / humidity / pressure),
publishes them to an MQTT broker over TLS, and renders a live
LVGL dashboard on a 240×320 ST7789.

## What it shows

- **BME280** environment sensor on I²C0.
- **TI CC3501E** Wi-Fi 6 + BLE 5.4 coprocessor for the network
  link through the AEN bridge backend.
- **OPTIGA Trust M** is the planned trust-anchor / TLS-handshake
  offload path; the current driver is probe-only, so this example
  still runs the TLS library through its software path.
- **`mbedtls`** supplies the TLS stack; MQTT currently enters through
  the portable `<alp/iot.h>` surface.  Hardware TLS backend entries remain
  `status: planned` in the library profile until a real mbedTLS
  consumer lands, so generated config emits the pure-C fallback
  instead of claiming CryptoCell / OPTIGA acceleration.
- **LVGL** composes the dashboard layout with sparkline chart +
  status strip.

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/connectivity/iot-dashboard
west flash
```

On `native_sim/native/64` the WiFi + MQTT paths stub via the
`<alp/iot.h>` NOSUPPORT contract; the UI still renders against the
sensor read.

## Customisation

Edit `src/main.c`:

- `alp_wifi_connect("ssid", "password")` -- your AP credentials.
- `alp_mqtt_open(...broker_uri = "mqtts://...")` -- your broker.

For production deployments, move credentials out of source into a
Zephyr settings backend (LittleFS-backed) or a sysbuild-time
KConfig override.

## Showcase pointers

This demo + the `production-deployment` example are the matched
pair: `iot-dashboard` shows the "happy path"; `production-
deployment` shows the same flow under firmware-update + remote
attestation + secure-boot constraints.
