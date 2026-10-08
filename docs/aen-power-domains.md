# AEN SoM power domains

Before STOP / STANDBY the SDK can hold the on-module consumers that would
otherwise burn power through the sleep, and put them back on the wake. This is
the runtime behind `alp_power_domain_policy_set()`, `alp_power_domain_info()` and
`alp_power_boot_wake_info()` in `<alp/power.h>` (issue #2784, unit U5). The STOP
backend ([below](#the-stop--standby-backend)) calls it; in RUN mode the pair is
exercised by `examples/aen/aen-power-domains` and `tests/unit/power_som_domains`.

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

The record lives in the 4 KB Utility SRAM ("BKRAM", base `0x4902C000`), which is
retained across STOP and reserved for the SDK: the E8 devicetree carries a `bkram`
node with `zephyr,memory-region = "ALP_BKRAM"`, so the linker emits a NOLOAD section
at that address that nothing zeroes at boot. Builds without that node (native_sim,
E4/E6) keep it in a `__noinit` cell, which does not survive STOP. The base address
comes from the Alif DFP `Backup_SRAM` memory entry (`start="0x4902C000"
size="0x1000"`) of the E1C, E3, E5 and E7 SVDs; the E8 SVD omits its `<memory>`
list, but its peripheral map leaves exactly that slot free and carries the block's
`BKRAM_CKEN` / `BKRAM_RET_MASK` fields. It is bench-unverified on the E8 (the BKRAM
pattern test in U8).

## The STOP / STANDBY backend

`src/backends/power/alif_se_power.c` implements `alp_power_request_sleep(STOP |
STANDBY)` on the E8 M55-HE (E1M-AEN801 / E1M-AEN803). It is registered for
`alif:ensemble:e8` behind `CONFIG_ALP_SDK_POWER_ALIF_SE` (default **n**,
experimental, **untested on silicon**). The M55-HE subsystem is powered off and the
wake is a cold boot through the Secure Enclave, so the call does not return: read
the cause with `alp_power_boot_wake_info()`. `SLEEP` / `DEEP_SLEEP` are forwarded to
the pm_policy backend when it is built.

**Wake sources** (only what is real is advertised):

| Bit | Armed by | Notes |
|---|---|---|
| `ALP_POWER_WAKE_RTC` | on-module RV-3028, `INT` -> P15_0 -> `WE_LPGPIO0` | primary RTC (the internal LPRTC is not trusted: ER001 / ER002, LFRC-only boot). With `wake_after_ms == 0` the caller's own alarm / countdown must already be armed, or the request is `ALP_ERR_INVAL`. |
| `ALP_POWER_WAKE_TIMER` | LPTIMER (`alp,power-wake-timer`, `WE_LPTIMER0`) for `wake_after_ms < 1000` | runs from the AON low-frequency clock: LFRC is ~4.5 % fast, so a short wake comes early by about that much. |
| (timed wake >= 1 s) | RV-3028 countdown (`rv3028c7_timer_start`) | whole seconds, rounded up; reports `ALP_POWER_WAKE_RTC`. Needs the chip context bound (`alp_som_power_bind_rv3028`). |

**The OFF profile is complete and explicit.** `se_service_set_run_cfg()` /
`set_off_cfg()` are not side-effect-free (bench: a one-field read-modify-write
dropped `memory_blocks` bit 20, cleared the retention LDO enables in
`VBAT_ANA_REG1` and the CVM masks in `RET_CTRL`), so every `off_profile_t` member is
assigned by name: `power_domains` (STOP: VBAT AON; STANDBY: SSE700 AON),
`dcdc_mode` OFF, `aon_clk_src` (LFXO only when `ANA.MISC_CTRL.SEL_32K` and
`XTAL32K_EN` already confirm it, else LFRC; with LFXO the 32 kHz crystal trim
`XTAL32K_CAP_CONT` is set to its maximum, 63), `memory_blocks` (BKRAM = gen2 bit 21,
plus the HE TCM banks the retention asks for), `vdd_ioflex_3V3` 1.8 V,
`wakeup_events` and `ewic_cfg` from the gen2 masks in `alif_aipm_gen2.h`, and
`vtor_address` / `vtor_address_ns` preserved from the live profile so the wake still
goes through SES -> ATOC. After the SE call the profile is read back, the retention
bits it needs in `RET_CTRL` / `VBAT_ANA_REG1` are re-asserted, and the request is
abandoned if any of it did not stick.

**Refusals**, all before any state is changed: `ALP_ERR_BUSY` when a debugger is
attached (`DHCSR.C_DEBUGEN`; bench override
`CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER`) or an armed source is already pending;
`ALP_ERR_NOSUPPORT` with the D-cache on (the clean loop hangs on this silicon);
`ALP_ERR_NOT_READY` when the core's low-power-state requests are not all OFF.

**Wake decode.** The record in BKRAM carries what was armed; on the cold boot the
LPTIMER status is read before its driver initialises, and the RV-3028 flags and
calendar in the I2C restore pass. `slept_ms` is the RV-3028 calendar delta (1 s
resolution).

**Not verified on silicon:** that the SE accepts the profile, that the EWIC entry
removes power (`RTSS_HE_CTRL.COLD_WAKEUP` is cleared and `WIC` set by
read-modify-write), that BKRAM retains with bit 21, the HE TCM bank sizes and
ITCM / DTCM split, STANDBY, and that the LPGPIO holds survive the SE's wake boot.
`examples/aen/aen-power-stop` is the bench for the first three.
