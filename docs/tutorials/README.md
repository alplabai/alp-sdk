# Tutorials

Companion walkthroughs for the example apps under `examples/`.
Each tutorial picks one example and explains it as a teaching
artefact: what the customer learns, the code path step-by-step,
and what to change to adapt it to their board.

Per the [`VERSIONS.md`](../../VERSIONS.md) v1.0 acceptance bar:
"Tutorials per library."  Curated set below; the rest of the
27 examples are reference material with their own per-directory
READMEs.

## Hand-picked tutorials

| Tutorial                                                                 | Difficulty   | Backs example / topic                          |
|--------------------------------------------------------------------------|--------------|------------------------------------------------|
| [01-first-build.md](01-first-build.md)                                   | beginner     | `gpio-button-led`                              |
| [02-i2c-scan.md](02-i2c-scan.md)                                         | beginner     | `i2c-scanner`                                  |
| [03-pwm-fade.md](03-pwm-fade.md)                                         | beginner     | `pwm-led-fade`                                 |
| [04-cross-family-portability.md](04-cross-family-portability.md)         | intermediate | `gpio-button-led` re-targeted to V2N           |
| [05-supervisor-mcu-bridge.md](05-supervisor-mcu-bridge.md)               | intermediate | `v2n-gd32-bridge-ping`                         |
| [06-secure-element-sign.md](06-secure-element-sign.md)                   | intermediate | `v2n-secure-element-sign`                      |
| [07-recovering-a-bricked-bridge.md](07-recovering-a-bricked-bridge.md)   | advanced     | `v2n-gd32-swd-flash`                           |
| [08-runtime-board-detection.md](08-runtime-board-detection.md)           | advanced     | `v2n-board-id-readout`                         |
| [09-board-yaml-deep-dive.md](09-board-yaml-deep-dive.md)                 | intermediate | `board.yaml` v2 schema reference (every block) |
| [10-secure-boot-signing.md](10-secure-boot-signing.md)                   | advanced     | MCUboot + ECDSA-P256 signing on AEN-Zephyr     |
| [11-mqtt-tls-publish.md](11-mqtt-tls-publish.md)                         | intermediate | `<alp/iot.h>` MQTT-over-TLS on Yocto           |
| [12-mender-ota.md](12-mender-ota.md)                                     | advanced     | Mender A/B-partition OTA on Yocto              |
| [13-eeprom-provisioning.md](13-eeprom-provisioning.md)                   | intermediate | On-module EEPROM manifest write (production)   |
| [14-audio-loopback.md](14-audio-loopback.md)                             | intermediate | `audio-loopback` (PDM → DSP → I²S)             |
| [15-mproc-mailbox.md](15-mproc-mailbox.md)                               | advanced     | `mproc-mailbox` (AEN M55-HP ↔ M55-HE IPC)      |
| [16-inference-mobilenet.md](16-inference-mobilenet.md)                   | intermediate | `<alp/inference.h>` MobileNet v2 on Ethos-U    |

## See also

* [`examples/README.md`](../../examples/README.md) -- full catalogue
  of every example app with one-line descriptions.
* [`docs/firmware-quickstart.md`](../firmware-quickstart.md) --
  cross-family patterns the tutorials build on.
