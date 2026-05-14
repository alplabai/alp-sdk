# 04 -- Cross-family portability

The core ALP-SDK promise: the same C source compiles for every
SoM family without source-level changes.  This tutorial retargets
`gpio-button-led` from AEN to V2N + i.MX 93 by editing only
`board.yaml`, and explains the portability rings the SDK enforces.

## The three rings

The SDK examples fall into three portability classes; the
`scripts/check_example_portability.py` lint enforces them:

| Ring                | Same source builds on...               | Example                                       |
|---------------------|----------------------------------------|-----------------------------------------------|
| 1 -- cross-family    | every E1M-X family                     | `gpio-button-led`, `i2c-scanner`, `pwm-led-fade` |
| 2 -- chip-bound      | every SoM that populates the chip      | `v2n-rtc-multi-alarm` (chip = `rv3028c7`)     |
| 3 -- som-bound       | one family only                        | `v2n-gd32-bridge-ping` (chip = `gd32g553`)    |

## Retarget Ring-1 -- the cheap path

```yaml
# board.yaml -- swap one line
som:
  sku: E1M-V2N101    # was E1M-AEN701
```

Rebuild:

```bash
west alp-build -b alp_e1m_evk_v2n examples/gpio-button-led
```

The SDK loader picks up the new `som.sku`, resolves the V2N preset
under `metadata/e1m_modules/E1M-V2N101.yaml`, regenerates
`alp.conf` with the V2N-specific Kconfig flags, and `west build`
drives the rest.

## Retarget Ring-2 -- read the chip families[]

If your example uses `<alp/chips/rv3028c7.h>`, the chip's
`metadata/chips/rv3028c7.yaml::families` list tells you which SoMs
work:

```yaml
families:
  - aen
  - v2n
  - v2n-m1
```

E1M-AEN701, E1M-V2N101, E1M-V2M101 all populate the RTC -- the
example runs unchanged.  E1M-NX9101 doesn't list `imx93` in this
chip's families; the build would fail.  The portability lint
catches this at PR time.

## Retarget Ring-3 -- you can't

`v2n-gd32-bridge-ping` uses the GD32G553 supervisor MCU, which
exists only on V2N.  AEN doesn't have it.  The example is named
`v2n-*` for exactly this reason -- the prefix is a contract.

## The runtime check

When your board.yaml declares a chip that's `assembled: optional`
in the SoM preset, the chip might not be physically populated on
every unit of that SKU.  Customer code must handle it:

```c
rv3028c7_t rtc;
alp_status_t s = rv3028c7_init(&rtc, bus);
if (s == ALP_ERR_NOT_READY) {
    printf("RTC not populated on this unit -- using software timer instead\n");
    /* fall back */
}
```

See [`docs/board-config.md`](../board-config.md) "Modular SoM" for
the full story on optional populations + `<alp/hw_info.h>`'s
runtime EEPROM-manifest readback.

## See also

* [`scripts/check_example_portability.py`](../../scripts/check_example_portability.py)
* [`docs/board-config.md`](../board-config.md)
* [Tutorial 08 -- runtime board detection](08-runtime-board-detection.md)
