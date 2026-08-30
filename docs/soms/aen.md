# E1M-AEN family

> Alif Ensemble-based SoMs in the E1M (35 × 35 mm) form factor.

## SKUs

| SKU            | Silicon                                | Status     |
|----------------|----------------------------------------|------------|
| `E1M-AEN301`   | Alif Ensemble E3                       | production |
| `E1M-AEN401`   | Alif Ensemble E4 (preliminary)         | preliminary|
| `E1M-AEN501`   | Alif Ensemble E5 (preliminary)         | preliminary|
| `E1M-AEN601`   | Alif Ensemble E6 (preliminary)         | preliminary|
| `E1M-AEN701`   | Alif Ensemble E7                       | production |
| `E1M-AEN801`   | Alif Ensemble E8 (preliminary)         | preliminary|

All AEN SKUs share the same module PCB.  Pick by Alif silicon
tier (cores + NPU count + memory).

## What's on the module

| Role                    | Part                       | Bus / signal     | Driver                                  |
|-------------------------|----------------------------|------------------|-----------------------------------------|
| Application SoC         | Alif Ensemble E3..E8       | --               | (vendor HAL)                            |
| Wi-Fi 6 + BLE 5.4       | TI CC3501E                 | inter-chip SPI1 + SDIO | App APIs: [`<alp/iot.h>`](../../include/alp/iot.h), [`<alp/ble.h>`](../../include/alp/ble.h); diagnostics: [`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h) |
| Secure element          | Infineon OPTIGA Trust M    | BRD_I2C†          | [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h) |
| RTC                     | Micro Crystal RV-3028-C7   | BRD_I2C†          | [`<alp/chips/rv3028c7.h>`](../../include/alp/chips/rv3028c7.h) |
| Temperature sensor      | TI TMP112                  | BRD_I2C†          | [`<alp/chips/tmp112.h>`](../../include/alp/chips/tmp112.h) |
| EEPROM (SoM manifest)   | Onsemi N24S128             | SoC I2C2 (bridge/DNP-selected, a separate bus from BRD_I2C) | [`<alp/chips/eeprom_24c128.h>`](../../include/alp/chips/eeprom_24c128.h) |
| Ethernet PHY            | TI DP83825 (exact order code TBD) | RMII      | none -- see [`metadata/chips/dp83825.yaml`](../../metadata/chips/dp83825.yaml) |

† On the **E1M-AEN801**, BRD_I2C is SoC I2C0 (function C, `P7_0`/`P7_1`),
master-capable, wired in the AEN801 board files (#1848) -- corrected from
an earlier belief that it was the slave-only Alif LPI2C0.  That routing is
R2-sourced (`E1M-AEN-2626-R2` netlist) and not yet confirmed on an actual
unit.  The other AEN SKUs still carry the pre-#1848 LPI2C0 assumption in
their own preset files pending the same netlist evidence.

Memory + per-SKU specifics: [`metadata/e1m_modules/E1M-AEN<NNN>.yaml`](../../metadata/e1m_modules/).

## CC3501E coprocessor

The AEN module's Wi-Fi 6 + BLE 5.4 ride a dedicated TI CC3501E
co-MCU rather than driver-level Wi-Fi on the Alif silicon.  Application
code stays on the portable APIs:

```c
alp_wifi_t *wifi = alp_wifi_open();
alp_ble_t *ble = alp_ble_open();
```

The AEN bridge helper initialises the CC3501E once and attaches its live handle
behind those dispatchers. Use
[`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h) and
[`alp companion`](../cc3501e-companion-commands.md) for diagnostics, raw scan
records, sockets, OTA, and bridge health.

Boot model + architecture: [`docs/cc3501e-bridge.md`](../cc3501e-bridge.md).

## Boot + identification

The SoM's identity comes solely from the on-module EEPROM manifest
(family / SKU / hw_rev / serial / mfg date) — there is no SoM-side
ADC cross-check.  A carrier-board BOARD_ID resistor divider, where
present, identifies the *carrier* revision and is independent of the
SoM revision; it is not yet wired into `alp_hw_info_read()`.  See
[`docs/board-id.md`](../board-id.md).

## Bring-up

The general bring-up procedure in
[`docs/getting-started.md`](../getting-started.md) covers AEN as
the first-class target.  AEN ships at v0.1; most cross-family
examples already work against it.

## Pins

* `metadata/e1m_modules/aen/from-alif.tsv` -- E1M pad → Alif silicon.
* `metadata/e1m_modules/aen/from-cc3501e.tsv` -- E1M pad → CC3501E MCU.
* `metadata/e1m_modules/aen/inter-chip.tsv` -- Alif ↔ CC3501E wiring.

## Example apps targeting AEN

All cross-family examples + the AEN-specific ones:

| Example                          | What you'll see                                             |
|----------------------------------|-------------------------------------------------------------|
| `edgeai-vision-aen`              | On-device inference with Ethos-U on AEN silicon.            |
| `iot-connected-camera`           | Capture + MQTT publish via the CC3501E.                     |

Plus every cross-family example.

## See also

* [`v2n.md`](v2n.md) -- larger form factor + Renesas-based.
* [`../cc3501e-bridge.md`](../cc3501e-bridge.md) -- inter-chip protocol.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
