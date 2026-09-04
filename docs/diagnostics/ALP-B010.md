# ALP-B010: peripheral kind not listed on the target silicon

A `cores.<id>.peripherals:` entry names a peripheral kind (`adc`, `can`,
`counter`, `dac`, `emmc`, `ethernet`, `flash`, `gpio`, `i2c`, `i2s`, `i3c`,
`pwm`, `rtc`, `sensor`, `spi`, `uart`, `usb`, `watchdog`) that doesn't show
up in the target SoC's own capability table
(`metadata/socs/<vendor>/<family>/<part>.json`, resolved from the SoM's
`silicon:` reference).

**This is a warning, not an error** -- it never fails `tan validate` or
blocks a build. SoC capability ingestion is incomplete for some parts (the
diagnostic itself may fire on a peripheral that silicon genuinely has), and
a few peripheral kinds (`emmc`, `flash`, `ethernet`) surface board-side via
an I/O controller rather than as a direct SoC block, so an ALP-B010 can be
a legitimate false positive. It's a "look at this" flag, not "this is
broken".

## Cause

- The core declares a peripheral kind the SoC JSON's `peripherals:` block
  either omits or lists with a zero count.
- The SoC's peripheral-count ingestion from the vendor reference manual is
  still incomplete for that part (a known gap on some SoCs).
- The peripheral is genuinely board-side (e.g. eMMC / flash / Ethernet
  reached through an I/O controller) rather than a SoC-internal block, so
  it was never going to show up in the SoC's own capability table.

## Diagnose

```sh
tan validate --format json --board-yaml board.yaml
```

Look for a `"severity": "warning"` entry with `"code": "ALP-B010"`; it
names the core, the peripheral kind, and the resolved silicon reference:

```
warning[ALP-B010]: core 'm33': peripheral kind 'can' is not listed on
  silicon 'nxp:imx9:imx93' (SoC JSON may be incomplete or the peripheral
  is board-side)
```

Cross-check the named SoC file directly (read-only) to see what it
currently claims:

```sh
cat metadata/socs/<vendor>/<family>/<part>.json   # e.g. metadata/socs/nxp/imx9/imx93.json
```

## Fix

Two different fixes depending on which side is stale:

- **The silicon genuinely lacks it**: drop the entry from
  `cores.<id>.peripherals:` (and from `populated:` if it also claims a
  board-side chip driver for it).
- **The SoC has it but the metadata is incomplete**: that's an SDK-side
  metadata gap, not a `board.yaml` problem -- update the SoC's
  `peripherals:` block in `metadata/socs/<vendor>/<family>/<part>.json`
  with the vendor reference-manual count.

## Escalate

Because this check is known to have false positives, don't silently
suppress a warning you've hardware-confirmed is wrong (the peripheral
really is present and really is board-side or SoC-internal) -- file an
issue against the SoC metadata so the capability table gets fixed for
every project targeting that silicon, not just yours.
