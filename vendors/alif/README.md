# vendors/alif

Vendor wrapper for the **Alif Ensemble** family (E1M-AEN).

The ALP SDK does not vendor Alif's HAL source — we link against
whatever revision the surrounding Zephyr tree pulls via
`modules/hal/alif`, or against a `cpackget`-installed
`AlifSemiconductor::Ensemble` CMSIS pack for plain-CMake / bare-metal
builds.

v0.1 targets the **latest stable Alif HAL revision supported by mainline
Zephyr**.  See `west.yml` and `docs/os-support-matrix.md` for the
exact pin once CI lands.

Implementation files (`i2c.c`, `spi.c`, `gpio.c`, `uart.c`, `display.c`)
will appear here when peripheral wrappers are signed off.
