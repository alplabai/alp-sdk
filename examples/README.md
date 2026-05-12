# Examples

Reference applications that double as tutorials.  Comment density
is ~50% in `src/main.c` so the source teaches by itself.  Pick the
example closest to what you want to do, copy the directory to your
project, and modify.

## Build any example

```bash
cd alp-workspace
west alp-build -b <board> alp-sdk/examples/<name>
west build -d build -t run            # native_sim
# or:
west flash                            # real silicon
```

`<board>` is your Zephyr board target.  Use `native_sim/native/64`
for host simulation (no hardware needed).  Real-silicon boards
match what [`docs/getting-started.md`](../docs/getting-started.md)
documents per SoM family.

## Cross-family examples

These work on every E1M-X SoM family.  Good starting points.

| Directory                    | What it shows                                                            |
|------------------------------|--------------------------------------------------------------------------|
| `gpio-button-led`            | GPIO open + configure (input + output); the canonical first build.       |
| `i2c-scanner`                | Walk an I2C bus + report devices that ACK.                               |
| `pwm-led-fade`               | PWM channel open + sweep duty cycle.                                     |
| `adc-voltmeter`              | Sample an ADC channel; convert raw → millivolts.                         |
| `uart-echo`                  | Open a UART; loop received bytes back to TX.                             |
| `uart-rx-ringbuf`            | Byte-granular IRQ-driven RX into a ring buffer (no polling).             |
| `rtc-clock`                  | Set + read the wall clock.                                               |
| `counter-alarm`              | Free-running counter + alarm callback at a configured value.             |
| `qenc-readout`               | Quadrature-encoder pulse counter.                                        |
| `wdt-feed`                   | Watchdog open + feed cadence; demonstrates reset-on-stall.               |
| `spi-loopback`               | SPI MOSI ↔ MISO loopback (jumper between the two pins).                  |
| `can-loopback`               | CAN(-FD) frame TX + self-reception via loopback mode.                    |
| `i2s-tone`                   | Generate a tone on an I2S DAC.                                           |
| `audio-loopback`             | PDM-in → I2S-out audio pass-through (low-latency).                       |
| `iot-connected-camera`       | End-to-end IoT: capture frame, publish to MQTT.                          |
| `edgeai-vision-aen`          | On-device inference with Ethos-U on an AEN SoM.                          |

## V2N / V2N-M1-specific examples

These exercise V2N-family-only components (the GD32 supervisor MCU,
the on-module PMIC fleet, the SoM EEPROM manifest).

| Directory                       | What it shows                                                            |
|---------------------------------|--------------------------------------------------------------------------|
| `v2n-gd32-bridge-ping`          | PING + GET_VERSION round-trip on both SPI fast path + I2C management path.|
| `v2n-board-id-readout`          | Read the SoM EEPROM manifest + assert the SKU matches the firmware build. |
| `v2n-pmic-rail-monitor`         | Poll ACT8760 + DA9292 + TPS628640 for fault state every second.          |
| `v2n-ethernet-dual`             | Bring up both RTL8211FDI PHYs (ET0 + ET1) -- probe, reset, autoneg, link, WoL. |
| `v2n-eeprom-manifest-dump`      | Hexdump + decode the 128-byte EEPROM manifest at offset 0x0000.          |
| `v2n-rtc-multi-alarm`           | Register per-source callbacks on the rv3028c7 multi-source dispatcher.   |
| `v2n-temp-sensor`               | Read the on-module TMP112 once per second; print degrees C.              |
| `v2n-pwm-fan-control`           | Ramp a GD32-side PWM channel along a five-stop fan curve (25 kHz carrier). |
| `v2n-secure-element-sign`       | OPTIGA Trust M init + product info + raw-APDU ECDSA-P256 sign.           |
| `v2n-xspi-flash-readwrite`      | Erase + write + read-back one page on the on-module xSPI NOR.            |
| `v2n-emmc-block-stat`           | Disk-access ioctls + first-block read on the on-module eMMC.             |
| `v2n-gd32-swd-flash`            | Host-driven SWD bit-bang -- connect, halt, erase, write, verify, reset.  |

## Anatomy of an example

Every example has the same shape:

```
examples/<name>/
├── CMakeLists.txt    # invokes scripts/alp_project.py + delegates to west build
├── prj.conf          # mostly empty -- feature selection is in board.yaml
├── board.yaml        # SoM + carrier + peripherals + chip drivers
├── src/
│   └── main.c        # the application code (heavily commented)
└── sample.yaml       # (optional) twister scenario metadata
```

To adapt to your own project:

1. Copy the directory.
2. Edit `board.yaml`:
   * `som.sku` -- your SoM MPN.
   * `carrier.name` -- your carrier preset (or inline `carrier.populated`).
   * `peripherals:` -- what your app uses.
   * `chips:` -- chip drivers to opt in.
3. Modify `src/main.c` to whatever your app needs.
4. `west alp-build -b <board> .` from your project directory.

## See also

* [`docs/firmware-quickstart.md`](../docs/firmware-quickstart.md) -- target-specific patterns.
* [`docs/board-config.md`](../docs/board-config.md) -- `board.yaml` schema reference.
* [`docs/troubleshooting.md`](../docs/troubleshooting.md) -- common errors + fixes.
