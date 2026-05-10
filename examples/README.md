# examples/

Reference applications shipped with the ALP SDK.  Each example is a
self-contained Zephyr application that builds against this repo as
a Zephyr module.  The repo ships **two flavours**:

- **End-to-end reference apps** (full pipelines for a product class).
- **Per-peripheral examples** (one-feature demos serving as
  hand-written templates for users writing their own apps).

## End-to-end reference apps

| Example                                            | Target board               | Status   | Notes                                                                                                  |
|----------------------------------------------------|----------------------------|----------|--------------------------------------------------------------------------------------------------------|
| [`edgeai-vision-aen/`](edgeai-vision-aen/)         | E1M EVK + E1M-AEN701       | skeleton | Camera → ISP/format-convert → Vela-compiled MobileNetV2 on Ethos-U → OLED overlay. v0.1 prints the init flow + the v0.2 pipeline TODOs; full impl in v0.2 alongside `<alp/inference.h>` Ethos-U path.  Twister gate: `[edgeai] done` line under `native_sim/native/64`. |
| [`iot-connected-camera/`](iot-connected-camera/)   | E1M EVK + E1M-V2N101/102   | skeleton | Camera (V2N MIPI CSI) → on-device DRP-AI3 classifier → MQTT/TLS publish to a broker, with a local LVGL UI.  Six-stage pipeline scaffolded in v0.1 (peripherals, camera, classifier, Wi-Fi station, MQTT, main loop); v0.2 fills the camera + classifier stages, v0.3 the IoT stages.  Twister gate: `[iotcam] done` line under `native_sim/native/64`. |

## Per-peripheral examples (v0.2)

One demo per wrapped peripheral class.  Each is a minimal,
hand-written Zephyr app — what you'd reach for as a starting
point when writing new firmware against the SDK without
alp-studio in the loop.  See [ADR 0001](../docs/adr/0001-wrapper-on-top-of-zephyr.md)
for why the standalone path is first-class.

| Example                                  | Header        | Demonstrates                                                            |
|------------------------------------------|---------------|-------------------------------------------------------------------------|
| [`gpio-button-led/`](gpio-button-led/)   | `peripheral.h` (GPIO) + `chips/button_led.h` | Button polling, LED toggle via the block helper.        |
| [`i2c-scanner/`](i2c-scanner/)           | `peripheral.h` (I²C) | 7-bit address scan via zero-length writes.                              |
| [`spi-loopback/`](spi-loopback/)         | `peripheral.h` (SPI) | Full-duplex transceive without a CS GPIO.                               |
| [`uart-echo/`](uart-echo/)               | `peripheral.h` (UART) | Read with timeout, mirror-write back.                                  |
| [`pwm-led-fade/`](pwm-led-fade/)         | `pwm.h`       | Linear duty sweep on PWM0 demonstrating `set_duty`.                     |
| [`adc-voltmeter/`](adc-voltmeter/)       | `adc.h`       | One-shot read in µV + capability-validation rejection of an oversized resolution. |
| [`counter-alarm/`](counter-alarm/)       | `counter.h`   | One-shot alarm at 100 ms via the counter API.                           |
| [`rtc-clock/`](rtc-clock/)               | `rtc.h`       | Set + get round-trip with the human-readable `alp_rtc_time_t`.          |
| [`wdt-feed/`](wdt-feed/)                 | `wdt.h`       | Install a 5 s timeout, feed it three times, demonstrate close.          |
| [`can-loopback/`](can-loopback/)         | `can.h`       | Loopback-mode TX + RX-callback dispatch.                                |
| [`i2s-tone/`](i2s-tone/)                 | `i2s.h`       | Stream a 1 kHz triangle wave (16-bit stereo) to ALP_E1M_I2S0.           |
| [`qenc-readout/`](qenc-readout/)         | `counter.h` (qenc side) | Poll ALP_E1M_ENC0's accumulated position.                       |

Every per-peripheral example asserts a `[<class>] done` line via
the twister console harness so the standalone-build path is gated
against silent regressions.

### What the native_sim runs prove

`native_sim` doesn't expose every peripheral controller — it has
emul drivers for I²C / SPI / GPIO / UART but not for PWM / ADC /
RTC / WDT / CAN / Counter.  On the latter set, `*_open` returns
NULL with `alp_last_error() == ALP_ERR_NOT_READY` and the example
exits after printing the diagnostic.  That's intentional: the
twister run proves **the wrapper plumbing compiles and links**,
without needing every backend to have an emulator.

Versions and acceptance criteria per example live in
[`../VERSIONS.md`](../VERSIONS.md).  CI runs each example's twister
scenario on every PR; the EdgeAI skeleton compiles under
`native_sim/native/64` until the EVK-AEN board file lands.

## Adding a new example

1. Create `examples/<name>/` with a Zephyr-app layout.
2. Add a row to the table above with a one-line summary.
3. Bind it to a `VERSIONS.md` deliverable so the acceptance bar is
   explicit.
4. Wire a `testcase.yaml` so twister picks it up — minimum gate is
   "compiles under `native_sim/native/64`."
