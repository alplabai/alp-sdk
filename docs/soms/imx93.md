# E1M-N93 family

> NXP i.MX 93-based SoMs in the E1M (35 × 35 mm) form factor.

## SKUs

| SKU            | Silicon                                | Status      |
|----------------|----------------------------------------|-------------|
| `E1M-NX9101`   | NXP i.MX 9352                          | preliminary |

i.MX 9352 = the top-of-line i.MX 93 SKU: dual Cortex-A55 @ 1.7 GHz
+ Cortex-M33 @ 250 MHz + Ethos-U65, no GPU.

## What's on the module

| Role                    | Part                       | Driver                                  |
|-------------------------|----------------------------|-----------------------------------------|
| Application SoC         | NXP i.MX 9352              | (vendor HAL via NXP MCUXpresso / Yocto) |
| PMIC                    | NXP PCA9451A               | (vendor HAL)                            |
| Wi-Fi 6 + BLE           | Murata Type 2DL/2EL/2KL/2LL (variant TBD) | (vendor HAL)             |
| Wi-Fi 6 (alt)           | NXP IW610                  | (vendor HAL)                            |

Exact populated variant: pending.  See [`metadata/e1m_modules/E1M-NX9101/som.yaml`](../../metadata/e1m_modules/E1M-NX9101/som.yaml).

## Boot + identification

Two-stage flow identical to the AEN + V2N families: EEPROM manifest
+ BOARD_ID ADC.  See [`docs/board-id.md`](../board-id.md).

## Bring-up

i.MX 93 support lands in the v0.3 cycle.  The cross-family
examples (GPIO, I2C, PWM, ADC, UART, RTC, …) target it through
the same `<alp/...>` API as the AEN family.

For inference workloads, the Ethos-U65 NPU is exposed through the
SDK's `<alp/inference.h>` dispatcher with the
`ALP_SDK_INFERENCE_ETHOS_U_N93` Kconfig.

## Pins

Pin maps land alongside the user-supplied authoritative HW config
writeup.  Track in [`metadata/e1m_modules/imx93/`](../../metadata/e1m_modules/imx93/).

## See also

* [`v2n.md`](v2n.md) -- E1M-X form factor, Renesas silicon.
* [`aen.md`](aen.md) -- E1M form factor, Alif silicon.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
