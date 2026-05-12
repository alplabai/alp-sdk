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

| Tutorial                                                                 | Difficulty | Backs example                                |
|--------------------------------------------------------------------------|------------|-----------------------------------------------|
| [01-first-build.md](01-first-build.md)                                   | beginner   | `gpio-button-led`                             |
| [02-i2c-scan.md](02-i2c-scan.md)                                         | beginner   | `i2c-scanner`                                 |
| [03-pwm-fade.md](03-pwm-fade.md)                                         | beginner   | `pwm-led-fade`                                |
| [04-cross-family-portability.md](04-cross-family-portability.md)         | intermediate | `gpio-button-led` re-targeted to V2N        |
| [05-supervisor-mcu-bridge.md](05-supervisor-mcu-bridge.md)               | intermediate | `v2n-gd32-bridge-ping`                      |
| [06-secure-element-sign.md](06-secure-element-sign.md)                   | intermediate | `v2n-secure-element-sign`                     |
| [07-recovering-a-bricked-bridge.md](07-recovering-a-bricked-bridge.md)   | advanced   | `v2n-gd32-swd-flash`                          |
| [08-runtime-board-detection.md](08-runtime-board-detection.md)           | advanced   | `v2n-board-id-readout`                        |

## See also

* [`examples/README.md`](../../examples/README.md) -- full catalogue
  of every example app with one-line descriptions.
* [`docs/firmware-quickstart.md`](../firmware-quickstart.md) --
  cross-family patterns the tutorials build on.
