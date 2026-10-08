# AEN SoM power domains

Before STOP / STANDBY the SDK can hold the on-module consumers that would
otherwise burn power through the sleep, and put them back on the wake. This is
the runtime behind `alp_power_domain_policy_set()`, `alp_power_domain_info()` and
`alp_power_boot_wake_info()` in `<alp/power.h>` (issue #2784, unit U5). The STOP
backend (U7) calls it; until then the pair is exercised in RUN mode by
`examples/aen/aen-power-domains` and `tests/unit/power_som_domains`.

## Domains and default (AUTO) actions

Domains are named by portable role. Presence, pads and the default action come
from the generated `alp,som-power-domain` nodes
(`metadata/e1m_modules/aen/on-module-links.yaml`), so a SKU that does not fit a
part has no node and the domain reads as absent.

| Domain | AUTO action | Comes back with |
|---|---|---|
| `WIFI_BLE` | hold `E_WIFI_NRST` (P15_1) low | nRESET release (`cc3501e_hard_reset()` semantics), then a PING before the driver context is re-armed |
| `ETH_PHY` | `E_PHY_PWRDWN` (P15_4) low; also tri-states the Y3 50 MHz reference oscillator | P15_4 high, then an `E_PHY_RESET` pulse |
| `EXT_FLASH` | take the `flash_ospi_alif` lock and wait for WIP to clear, then hold `OSPI1_RESETn` (P15_7) low | release, then the driver drops its Octal DDR state and unlocks |
| `EXT_RAM` | hold `OSPI0_RESETn` (P15_6) low | release |
| `TEMP_SENSOR` | TMP112 `CONFIG.SD` | clear `SD` |
| `RTC` | RV-3028 stays powered; CLKOUT low, `CONTROL_1.EERD` set so the 24 h EEPROM refresh cannot switch CLKOUT back on | EERD back to its pre-quiesce value (CLKOUT stays low) |
| `BACKLIGHT` | `BACKLIGHT_EN` (P5_5) low (a main-domain pad, does not hold through STOP) | back to its previous level |

`flash_ospi_alif` exposes no power-management or deep-power-down hook and no
HyperRAM driver exists, so both memories use reset-hold. The Wi-Fi restore never
uses `cc3501e_reset()`: that call drops `WIFI_EN` for 50 ms (a supply cold
cycle), which on an activated unit may never relaunch the firmware.

## Policy

`AUTO` (default) applies the action above, `KEEP_ALIVE` never touches the
domain, and `RAIL_OFF` gates the supply. Only `WIFI_BLE` has a rail-off action,
and only with `CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF=y` plus the runtime policy;
every other request answers `ALP_ERR_NOSUPPORT`, and a domain the SKU does not
carry answers `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`.

## Quiesce, record, restore

- `alp_som_power_quiesce(mode)` runs consumers first (Wi-Fi, flash, RAM,
  temperature sensor, RTC, backlight, PHY) and writes a record (magic + CRC +
  the quiesced set). If one domain fails, the ones already held are restored.
- `alp_som_power_restore()` walks the record in reverse and never stops on a
  failure; failed domains are reported.
- A driver that owns a domain is called through a hook (`som_power_chips.h`:
  CC3501E, TMP112, RV-3028; the OSPI flash binds itself), so its own state stays
  correct. Without a hook the layer drives the devicetree pins and I2C address.
- Power state is tracked in software: an unpowered PHY reads stale MDIO data,
  not `0xFFFF`.
- Restore after a wake is two `SYS_INIT` passes: pin domains at `POST_KERNEL` 0,
  the I2C-backed ones (TMP112, RV-3028) at priority 51 once `i2c_dw` is up and
  before the sensor and Ethernet drivers. Both run only when the record is valid
  (and `STOP_MODE_STAT` agrees); on a plain POR nothing is touched.
- The pads are muxed and pad-configured by the generated `alp,som-power` node's
  `pinctrl-0` group (input buffer on), applied before the first drive; the layer
  refuses to drive without it. Every hold is read back and a mismatch fails the
  quiesce, so the caller never sleeps on a hold that did not happen. The LP-pad
  register layout comes from the Zephyr Alif pinctrl driver
  (`drivers/pinctrl/pinctrl_alif.c`, `soc/alif/ensemble/pinctrl_soc.h`) and is
  TBD against the HWRM.
- A quiesce that fails rolls back the failing domain's own partial step and the
  domains already held, and reports rollback failures to the caller; the record
  kept for the retry is a RUN-mode record, so a warm reset still restores it.
- After the CC3501E restore the chip is as after a reset: Wi-Fi association,
  sockets and BLE state are gone and the application re-establishes them.
- RUN-mode cycles: the PHY's 50 MHz reference oscillator stops with the PHY, so
  `net_if_down()` the Ethernet interface before the cycle.

The record is held behind a store abstraction backed by a `__noinit` RAM
placeholder. It does not survive STOP; BKRAM placement is a U7 item.
