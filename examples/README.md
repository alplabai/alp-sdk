@page examples_index Examples

# Examples

Reference applications that double as tutorials.  Comment density
is ~50% in `src/main.c` so the source teaches by itself.  Pick the
example closest to what you want to do, copy the directory to your
project, and modify.

## Build any example

```bash
cd alp-workspace
west alp-build alp-sdk/examples/<name>
```

`west alp-build` reads the example's `board.yaml` v2, resolves the
SoM topology, and fans out per-core slices (Zephyr / Yocto /
baremetal).  Single-OS examples fan out into one slice; heterogeneous
examples fan out into multiple slices in parallel.  See
[`docs/heterogeneous-builds.md`](../docs/heterogeneous-builds.md)
for the dual-OS walk-through and
[`docs/firmware-quickstart.md`](../docs/firmware-quickstart.md) for
the Zephyr-only fast lane.

Real-silicon boards match what
[`docs/getting-started.md`](../docs/getting-started.md) documents
per SoM family; `native_sim/native/64` covers host simulation when
no hardware is plugged in.

## Find an example by what it does

New to the SDK?  Build `hello-world`, then `gpio-button-led` -- the
two-minute "is my toolchain alive?" path.  After that, jump to the
group that matches your peripheral or use case.

Most examples are portable across E1M-X SoM families.  A bold tag
marks the ones locked to specific silicon:

* **(AEN)** -- Alif Ensemble family (Ethos-U NPU, dual-M55).
* **(V2N)** -- Renesas RZ/V2N.
* **(V2N-M1)** -- V2N plus the DEEPX DX-M1 NPU.
* **(iMX93)** -- NXP i.MX93.
* **(E1M-X)** -- E1M-X form-factor pad / peripheral.

Many v0.5--v0.6 examples are still `[UNTESTED]` -- they build on
`native_sim/native/64` and cross-compile, but await hardware-in-the-
loop bring-up.  Each example's own `README.md` carries its status.

### Peripheral I/O

The portable bus + GPIO + analog surfaces.  Start here.

| Directory                    | What it shows                                                            |
|------------------------------|--------------------------------------------------------------------------|
| `hello-world`                | Minimal "first program" -- no peripherals; just a printf heartbeat.       |
| `gpio-button-led`            | GPIO open + configure (input + output); the canonical first build.       |
| `i2c-scanner`                | Walk an I2C bus + report every device that ACKs.                         |
| `i2c-master`                 | Read a known I2C device (TMP112) at a known address.                     |
| `i2c-slave`                  | Slave-mode shape + SDK gap notice (master-only today; v0.7 lands slave). |
| `spi-master`                 | Discrete SPI master -- write / transceive / read patterns.                |
| `spi-slave`                  | Slave-mode shape + SDK gap notice (master-only today; v0.7 lands slave). |
| `spi-loopback`               | SPI MOSI ↔ MISO loopback (jumper between the two pins).                  |
| `uart-echo`                  | Open a UART; loop received bytes back to TX.                             |
| `uart-hello-world`           | Canonical "printf via UART" walkthrough -- producer-only counterpart.     |
| `uart-rx-ringbuf`            | Byte-granular IRQ-driven RX into a ring buffer (no polling).             |
| `pwm-led-fade`               | PWM channel open + sweep duty cycle.                                     |
| `adc-voltmeter`              | Sample an ADC channel; convert raw → millivolts.                         |
| `vendor-ext-composability`   | One ADC pad, three ways -- plain GPIO, portable ADC, then an *additive* Alif vendor extension. Proves `<alp/ext/...>` never locks a pad out of the portable surfaces. |
| `dac-waveform`               | Generate a sine wave on `E1M_X_DAC0`. **(E1M-X; V2N today)**             |
| `can-loopback`               | CAN(-FD) frame TX + self-reception via loopback mode.                    |
| `qenc-readout`               | Quadrature-encoder pulse counter.                                        |
| `drone-autopilot`            | Real-time PID flight controller -- IMU → cascaded PID → PWM ESCs; deterministic multi-rate loops. **(AEN; advanced -- don't fly)** |

### Audio

| Directory                   | What it shows                                                               |
|-----------------------------|-----------------------------------------------------------------------------|
| `i2s-tone`                  | Generate a tone on an I2S DAC.                                              |
| `audio-loopback`            | PDM-in → I2S-out audio pass-through (low-latency).                          |
| `audio-noise-suppression`   | Real-time denoise -- I2S mic → FFT → int8 CNN gain mask → I2S out. **(V2N)** |
| `audio-wake-word`           | Always-on "Hey Alp" keyword spotting on the M55-HE core + Ethos-U55. **(AEN)** |

### Camera / Vision

| Directory                       | What it shows                                                                 |
|---------------------------------|-------------------------------------------------------------------------------|
| `ai-camera-viewer`              | Capture frames → person-detect on the Ethos-U NPU → preview + boxes on a 240×240 LVGL display. **(AEN)** |
| `ai-object-detection-realtime`  | Realtime YOLOv8-tiny on the DEEPX NPU -- camera → inference → bbox overlay + live FPS. **(V2N-M1)** |

### AI / Inference

| Directory                          | What it shows                                                                |
|------------------------------------|------------------------------------------------------------------------------|
| `ai-anomaly-detection-vibration`   | Predictive maintenance -- sliding-window vibration → 1D-CNN anomaly score via TFLM. **(AEN)** |
| `aen/edgeai-vision-aen`            | On-device vision inference with Ethos-U on an AEN SoM. **(AEN)**              |
| `v2n/v2n-m1-deepx-inference`       | DEEPX DX-M1 NPU bring-up + a single inference through `<alp/inference.h>`. **(V2N-M1)** |
| `v2n/v2n-m1-ros-perception`        | ROS 2 perception node -- detection on DEEPX, DRP-AI3 fallback on plain V2N. **(V2N / V2N-M1, Yocto)** |

### Connectivity / IoT

| Directory                | What it shows                                                                     |
|--------------------------|-----------------------------------------------------------------------------------|
| `iot-connected-camera`   | End-to-end IoT -- capture a frame, publish it to MQTT.                             |
| `iot-dashboard`          | BME280 env samples → MQTT-over-TLS publish + a live LVGL dashboard. **(AEN)**      |
| `iot-fleet-ota`          | Secure OTA firmware update with rollback; the v0.6 declarative `boot:` + `ota:` reference. |
| `production-deployment`  | Field-product lifecycle flagship -- secure-boot + OTA + EEPROM provisioning + attestation in one `board.yaml`. |

### Display / GUI

Each needs a display panel wired per its `board.yaml`.

| Directory             | What it shows                                                               |
|-----------------------|-----------------------------------------------------------------------------|
| `lvgl-widgets-demo`   | Upstream `lv_demo_widgets()` -- every standard LVGL widget; app stays board-portable. |
| `lvgl-benchmark`      | Upstream `lv_demo_benchmark()` -- per-scene FPS; shows GPU2D/DMA2D accel on capable SoMs. |
| `lvgl-music-player`   | Upstream `lv_demo_music()` UI paired with the SDK audio chain (I2S + WM8960 codec). **(AEN)** |
| `drone-hud`           | Full drone telemetry HUD on a 240×320 TFT, fusing IMU + GNSS + battery via `madgwick_ahrs`. **(AEN)** |

### Power / Timing

| Directory                  | What it shows                                                               |
|----------------------------|-----------------------------------------------------------------------------|
| `rtc-clock`                | Set + read the wall clock.                                                  |
| `counter-alarm`            | Free-running counter + alarm callback at a configured value.                |
| `timer-periodic-interrupt` | Re-arming periodic alarm + ISR-safe coordination pattern (flag + drain).    |
| `wdt-feed`                 | Watchdog open + feed cadence; demonstrates reset-on-stall.                  |
| `power-managed-sensor`     | Low-power BME280 + IMU node -- the v0.6 `cores.<id>.power:` sleep + multi-source-wakeup reference. **(AEN)** |

## Browse by topology / platform

The remaining examples group by **how they are built** rather than
by peripheral -- multi-core projects, and SoM-specific board bring-up.

### Multi-core & heterogeneous-OS

These declare more than one core in `board.yaml` and exercise the
v0.6 orchestrator's per-core fan-out.  Each carries a per-core
subdirectory layout (`linux/`, `m33_sm/`, `m55_hp/`, ...) matching
its `cores:` keys.

| Directory                | What it shows                                                                                |
|--------------------------|----------------------------------------------------------------------------------------------|
| `rpmsg-v2n`              | V2N flagship -- A55 Yocto consumer + M33-SM Zephyr producer, framed RPC over RPMsg. **(V2N)** |
| `rpmsg-aen`              | AEN E7 -- A32 Yocto consumer + M55-HP Zephyr producer reading on-board IMU + barometer. **(AEN)** |
| `rpmsg-imx93`            | iMX93 -- A55 Yocto consumer + M33 Zephyr producer (structural; build pending iMX93 HW map). **(iMX93)** |
| `heterogeneous-offload`  | "Why heterogeneous compute?" -- A55 delegates a 1024-pt FFT to M33-SM via `alp_rpc_call`.     |
| `mproc-mailbox`          | M55-HP ↔ M55-HE mailbox round-trip -- stage payload in shared SRAM, signal via HW mailbox, read the reply. **(AEN)** |

### V2N platform

These live under `examples/v2n/` and exercise V2N-family-only
components (the GD32 supervisor MCU, the on-module PMIC fleet, the
SoM EEPROM manifest).

| Directory                       | What it shows                                                            |
|---------------------------------|--------------------------------------------------------------------------|
| `v2n/v2n-gd32-bridge-ping`      | PING + GET_VERSION round-trip on both SPI fast path + I2C management path.|
| `v2n/v2n-board-id-readout`      | Read the SoM EEPROM manifest + assert the SKU matches the firmware build. |
| `v2n/v2n-ethernet-dual`         | Bring up both RTL8211FDI PHYs (ET0 + ET1) -- probe, reset, autoneg, link, WoL. |
| `v2n/v2n-eeprom-manifest-dump`  | Hexdump + decode the 128-byte EEPROM manifest at offset 0x0000.          |
| `v2n/v2n-rtc-multi-alarm`       | Register per-source callbacks on the rv3028c7 multi-source dispatcher.   |
| `v2n/v2n-temp-sensor`           | Read the on-module TMP112 once per second; print degrees C.              |
| `v2n/v2n-pwm-fan-control`       | Ramp a GD32-side PWM channel along a five-stop fan curve (25 kHz board). |
| `v2n/v2n-secure-element-sign`   | OPTIGA Trust M init + product info + raw-APDU ECDSA-P256 sign.           |
| `v2n/v2n-xspi-flash-readwrite`  | Erase + write + read-back one page on the on-module xSPI NOR.            |
| `v2n/v2n-emmc-block-stat`       | Disk-access ioctls + first-block read on the on-module eMMC.             |
| `v2n/v2n-gd32-swd-flash`        | Host-driven SWD bit-bang -- connect, halt, erase, write, verify, reset.  |

## Anatomy of a single-OS example

```
examples/<name>/
├── CMakeLists.txt    # invokes scripts/alp_project.py + delegates to west build
├── prj.conf          # mostly empty -- feature selection is in board.yaml
├── board.yaml        # SoM + board + cores + peripherals + chip drivers
├── src/
│   └── main.c        # the application code (heavily commented)
└── testcase.yaml     # (optional) twister scenario metadata
```

## Anatomy of a heterogeneous example

```
examples/<name>/
├── CMakeLists.txt        # multi-slice project marker (no add_subdirectory)
├── board.yaml            # declares 2+ cores under `cores:` + cross-core `ipc:`
├── README.md
├── <a-core-id>/          # one sub-dir per `cores.<id>` (e.g. `linux/`)
│   ├── CMakeLists.txt
│   └── src/main.c
└── <m-core-id>/          # (e.g. `m33_sm/`, `m55_hp/`)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c
```

To adapt to your own project:

1. Copy the directory.
2. Edit `board.yaml`:
   * `som.sku` -- your SoM MPN.
   * `board.name` -- your board preset (or inline `board.populated`).
   * `cores.<id>.peripherals` -- what each core uses.
   * `cores.<id>.libraries` -- per-core library knobs.
   * Heterogeneous projects: `ipc:` -- name a carve-out the
     orchestrator allocates from the SoM's `memory_map:`.
3. Modify each core's `src/main.c` to whatever your app needs.
4. `west alp-build .` from your project directory.

## See also

* [`docs/firmware-quickstart.md`](../docs/firmware-quickstart.md) -- single-OS patterns.
* [`docs/heterogeneous-builds.md`](../docs/heterogeneous-builds.md) -- per-core walk-through.
* [`docs/board-config.md`](../docs/board-config.md) -- `board.yaml` schema reference.
* [`docs/troubleshooting.md`](../docs/troubleshooting.md) -- common errors + fixes.
