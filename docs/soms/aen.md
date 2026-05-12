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
| Wi-Fi 6 + BLE 5.4       | TI CC3501E                 | inter-chip SPI1 + SDIO | [`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h) |
| Secure element          | Infineon OPTIGA Trust M    | LPI2C            | [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h) |
| RTC                     | Micro Crystal RV-3028-C7   | LPI2C            | [`<alp/chips/rv3028c7.h>`](../../include/alp/chips/rv3028c7.h) |
| Temperature sensor      | TI TMP112                  | LPI2C            | [`<alp/chips/tmp112.h>`](../../include/alp/chips/tmp112.h) |
| EEPROM (SoM manifest)   | Onsemi N24S128             | LPI2C            | [`<alp/chips/eeprom_24c128.h>`](../../include/alp/chips/eeprom_24c128.h) |
| Ethernet PHY            | TI DP83825IRMQR            | RMII             | (Zephyr-side PHY driver)                |

Memory + per-SKU specifics: [`metadata/e1m_modules/E1M-AEN<NNN>/som.yaml`](../../metadata/e1m_modules/).

## CC3501E coprocessor

The AEN module's Wi-Fi 6 + BLE 5.4 ride a dedicated TI CC3501E
co-MCU rather than driver-level Wi-Fi on the Alif silicon.  The
host driver speaks the documented inter-chip protocol over SPI1:

```c
alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
    .bus_id = ALP_E1M_SPI1, .freq_hz = 8000000,
    .mode = ALP_SPI_MODE_0, .bits_per_word = 8,
});
cc3501e_t fw;
cc3501e_init(&fw, bus);
cc3501e_reset(&fw);
```

Boot model + APIs: [`docs/cc3501e-bridge.md`](../cc3501e-bridge.md).
Driver: [`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h).

## Boot + identification

Same two-stage flow as V2N: EEPROM manifest + BOARD_ID ADC.  See
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
