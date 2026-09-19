# iot-dashboard

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Builds clean on
> `native_sim/native/64`; real WiFi + MQTT + TLS bring-up gates on
> v0.6 AEN HiL.

Customer-facing IoT demo: an E1M-AEN module reads BME280
environment samples (temperature / humidity / pressure),
publishes them to an MQTT broker over TLS, and renders a live
LVGL dashboard on a RK055HDMIPI4MA0 720×1280 MIPI-DSI panel
(Himax HX8394) on the EVK's J6 connector.

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
  status strip, bound to the panel via `<alp/display.h>` +
  `alp_gui_lvgl_attach()` (`<alp/gui.h>`) -- no direct
  `<zephyr/drivers/display.h>` calls in app code.  The dashboard's own
  layout is fixed at 240×320 and draws in the top-left corner of the
  720×1280 panel; it does not scale to fill it (#2173).

## Build

A real-silicon build refuses to configure without a real entropy
source (#2192: AEN has no Ensemble entropy driver yet, so the weak-RNG
guard in `zephyr/CMakeLists.txt` trips), so the plain command below
fails at configure time without the opt-in shown. CI's `testcase.yaml`
opts in the same way to get a build-only compile check; the
`CONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y` acknowledgement is
**bench-only -- never ship it**.

Compile-only check (matches CI, no MCUboot signing, not flashable):

```
west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp \
    examples/connectivity/iot-dashboard -- \
    -DCONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y   # bench-only -- never ship this flag
```

The documented AEN flow builds with `--sysbuild` instead, so MCUboot
signs the image into slot0 (see `docs/_aen-runbook-section.md`). Under
`--sysbuild`, sysbuild does not forward a plain `-DBOARD=`/`-DCONFIG_...`
pair to the app image's own CMake invocation the way a non-sysbuild
build does, so the config override below must be passed unprefixed at
the sysbuild command line, which sysbuild then routes to the main app
image (`iot-dashboard`) only:

```
west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp \
    examples/connectivity/iot-dashboard --sysbuild -- \
    -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf \
    -DCONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y   # bench-only -- never ship this flag
west flash
```

(`alp_e1m_aen801_m55_hp/...` also builds -- pick the target matching
your SoM SKU.) `CMakeLists.txt` applies the `e1m_evk_rk055hdmipi4ma0`
shield automatically for these two AEN M55-HP board targets under
either build form, so no `-DSHIELD=...` flag is needed.

> The display chain above is compile-proven, not bench-proven: no
> pixels have been confirmed on glass yet. It depends on #2204 (the
> `e1m_evk_rk055hdmipi4ma0` shield), and the twister coverage this
> example gets is `build_only`.

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
