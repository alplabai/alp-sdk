# iot-dashboard

> ⚠️ **`[UNTESTED]` on hardware.** Builds clean on
> `native_sim/native/64`; real WiFi + MQTT + TLS bring-up still needs
> AEN HiL.

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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_he` here, in `build/m55_he-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

AEN hardware builds seed the PSA core from the Secure Enclave TRNG entropy
driver (`alif,se-trng`, issue #2192), which every AEN board chooses as
`zephyr,entropy` by default, so no weak-RNG opt-in is needed. Only the
native_sim build, which has no real entropy source, sets
`CONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y` (in `native_sim.conf`).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```
# Compile-only check (matches CI, no MCUboot signing, not flashable):
# writes examples/connectivity/iot-dashboard/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/connectivity/iot-dashboard
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
    examples/connectivity/iot-dashboard -- \
    -DEXTRA_CONF_FILE=generated/alp.conf

# Documented AEN flow: --sysbuild, so MCUboot signs the image into slot0
# (see docs/_aen-runbook-section.md).  Sysbuild does not forward a plain
# -DBOARD=/-DCONFIG_... pair to the app image's own CMake invocation, so the
# extra conf file is passed with the app-image prefix on the sysbuild
# command line, which routes it to the main app image (iot-dashboard) only:
python3 scripts/gen_example_alp_conf.py examples/connectivity/iot-dashboard
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp \
    examples/connectivity/iot-dashboard --sysbuild -- \
    -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf \
    -Diot-dashboard_EXTRA_CONF_FILE=generated/alp.conf
west flash
```

`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp` also builds, for the
AEN803 twin of this SoM -- pick the target matching your SoM SKU.
`CMakeLists.txt` applies the `e1m_evk_rk055hdmipi4ma0` shield
automatically for these two AEN M55-HP board targets under either build
form, so no `-DSHIELD=...` flag is needed.

> The display chain above is compile-proven, not bench-proven: no
> pixels have been confirmed on glass yet. It depends on #2204 (the
> `e1m_evk_rk055hdmipi4ma0` shield), and the twister coverage this
> example gets is `build_only`. This app's M55-HP display chain is
> compile-checked only -- the shield's own glass evidence (#2204,
> E1M-AEN803 2026W36-0009) was an M55-HE run -- and the
> `boards/*.overlay` CC3501E bridge wiring these AEN targets need is an
> HP twin of the bench-proven M55-HE wiring, itself unbenched.

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
