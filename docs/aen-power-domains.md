# AEN SoM power domains

Before STOP / STANDBY the SDK can hold the on-module consumers that would
otherwise burn power through the sleep, and put them back on the wake. This is
the runtime behind `alp_power_domain_policy_set()`, `alp_power_domain_info()` and
`alp_power_boot_wake_info()` in `<alp/power.h>` (issue #2784, unit U5). The STOP
backend (section "The STOP / STANDBY backend" below) calls it; in RUN mode the pair is
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
E4/E6) keep it in a `__noinit` cell, which does not survive STOP. The base address is
bench-verified on the E8 (E1M-AEN803 on an E1M-EVK, RAM-run, 2026-10-09): 4 KiB
read/write at `0x4902C000..0x4902CFFF`, no alias at +0x800, PDM at `0x4902D000`
unaffected, contents survive `AIRCR.SYSRESETREQ` and are lost on a cold power cycle.
It also matches the `Backup_SRAM` memory entry of the E1C, E3, E5 and E7 DFP SVDs
(the E8 SVD omits its `<memory>` list). The clock gate is `BKRAM_CKEN`
(`CLKCTL_PER_SLV` `0x4902F000` bit 4, set at cold boot; asserted before every
access) and retention is `VBAT.RET_CTRL` (`0x1A60900C`) bit 0 `BKRAM_RET_MASK`,
bit 1 `BKRAM_RET_FORCE`. Survival across STOP itself is the U8 bench test; E4 is
unverified.

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
| `ALP_POWER_WAKE_TIMER` | LPTIMER (`alp,power-wake-timer`, `WE_LPTIMER0`) for `wake_after_ms < 1000` | runs from the AON low-frequency clock. `wake_after_ms` is a **minimum**: the tick count is rounded up against the fastest the clock can run (LFRC 36045 Hz = +10 %, the top of its trim range; measured 34251.7 Hz; LFXO 32775 Hz), so on LFRC a short wake is never early but can be late: ~5 % at the 34251.7 Hz this module measured, up to ~16 % if the clock sits at the bottom of the trim range (-5 %, ~31130 Hz). |
| (timed wake >= 1 s) | RV-3028 countdown (`rv3028c7_timer_start`) | whole seconds, rounded up. Needs the chip context bound (`alp_som_power_bind_rv3028`). |

A timed wake reports the source the caller asked for, whichever hardware serves it:
`ALP_POWER_WAKE_TIMER` (also when only `wake_after_ms` was given), or
`ALP_POWER_WAKE_RTC` when only `ALP_POWER_WAKE_RTC` was configured. The RV-3028
`INT` pad is armed as a falling-edge interrupt, inside the interrupt-off entry
section, so the edge is latched even by a short pulse (the part's pulse mode,
tRTN1 = 7.8 ms, is never enabled by the SDK: `INT` stays low until the wake decode
clears the flag). The LPGPIO combined line (IRQ 57) is the one that reaches the
EWIC; its handler only masks the line again.

**The OFF profile is complete and explicit.** `se_service_set_run_cfg()` /
`set_off_cfg()` are not side-effect-free (bench: a one-field read-modify-write
dropped `memory_blocks` bit 20, cleared the retention LDO enables in
`VBAT_ANA_REG1` and the CVM masks in `RET_CTRL`), so every `off_profile_t` member is
assigned by name: `power_domains` (STOP: VBAT AON; STANDBY: VBAT AON + SSE700 AON),
`dcdc_mode` OFF (STANDBY adds PD2 to PD0), `aon_clk_src` (LFXO only when `ANA.MISC_CTRL.SEL_32K` and
`XTAL32K_EN` already confirm it, else LFRC; with LFXO the 32 kHz crystal trim
`XTAL32K_CAP_CONT` is set to its maximum, 63), `memory_blocks` (BKRAM = gen2 bit 21 and
MRAM \| SERAM, plus the HE TCM banks the retention asks for), `vdd_ioflex_3V3` 1.8 V,
`wakeup_events` and `ewic_cfg` from the gen2 masks in `alif_aipm_gen2.h`, and
`vtor_address` / `vtor_address_ns` = `SCB->VTOR` (the vendor resume vector; bench U8g: with
this and MRAM \| SERAM in `memory_blocks` STOP woke 2 of 2, without both it never did). After the SE call the profile is read back, the retention
bits it needs in `RET_CTRL` / `VBAT_ANA_REG1` are re-asserted, and the request is
abandoned if any of it did not stick. All 14 members are compared on the readback.
`RET_CTRL` and `VBAT_ANA_REG1` are snapshotted before the SE call; every later exit
(a failure, or a sleep that did not power down) writes the live profile and both
snapshots back and verifies them.

**Every pad the layer drives needs its GPIO controller enabled in the devicetree**
(`&gpio5` for the backlight enable, `&gpio11` for the PHY reset, `&lpgpio` for the
P15_n pads); a disabled controller makes that domain's quiesce fail with
`ALP_ERR_NOT_READY` and the sleep is refused (bench U8). Every refusal prints
`alif_se_power: refuse step=<n> reason=<...> rc=<raw>` on the console.

**Refusals**, all before any state is changed: `ALP_ERR_BUSY` when a debugger is
attached (`DHCSR.C_DEBUGEN`; bench override
`CONFIG_ALP_SDK_POWER_ALIF_SE_ALLOW_DEBUGGER`) or an armed source is already pending;
`ALP_ERR_NOSUPPORT` with the D-cache on (the clean loop hangs on this silicon);
`ALP_ERR_NOT_READY` when the core's low-power-state requests are not all OFF.

**Entry.** Interrupts are off for the sequence (`PRIMASK`, `BASEPRI` 0, as the DFP
does) and the wake pad is armed and disarmed inside it. A sleep that returns with a
fired source is reported as an early wake; one that returns with none is
`ALP_ERR_IO`, with the SE profile and the domains put back.

**A STOP wake with no usable record** (the SRAM did not keep it, or the CRC fails)
is not left with held domains: if `STOP_MODE_STAT` says STOP, every present pad is
released to its inactive level (a blind early restore; no rail-off, backlight stays
off) and `alp_power_boot_wake_info()` reports a STOP cycle with no wake cause. A
STANDBY record is discarded when the RV-3028 reports its power-on-reset flag.
The BKRAM placement exists only in a build with `CONFIG_ALP_SDK_POWER_ALIF_SE`;
every other build keeps the record in RAM.

### Comparison with the vendor reference (bench U8c)

The vendor `system_off` sample (sdk-alif `samples/drivers/pm/system_off`, whose MRAM-boot
SOFT_OFF is the same `PD_VBAT_AON` profile as our STOP) and the Bluetooth `power_mgr.c`
set the OFF profile as follows. Differences against `build_off_profile()`:

| Member | This backend (STOP) | Vendor `system_off` (MRAM boot) | Vendor BLE `power_mgr` (STOP) |
|---|---|---|---|
| `power_domains` | `PD_VBAT_AON` | `PD_VBAT_AON` | `PD_VBAT_AON` |
| `dcdc_voltage` / `dcdc_mode` | live (825) / OFF | 825 / OFF | 775 / OFF |
| `aon_clk_src` | **LFRC** unless confirmed | **LFXO** | **LFXO** |
| `stby_clk_src` | HFRC | HFRC | HFRC |
| `stby_clk_freq` | `RC_STDBY_0_075` | **`RC_STDBY_76_8`** | `RC_STDBY_0_075` |
| `memory_blocks` | BKRAM (bit 21) \| MRAM \| SERAM, plus TCM on request | **`MRAM_MASK` \| `SERAM_MASK`** | MRAM \| SERAM \| retention blocks |
| `ip_clock_gating` / `phy_pwr_gating` | 0 / 0 | 0 / 0 | 0 / 0 |
| `vdd_ioflex_3V3` | 1.8 V | 1.8 V | 1.8 V |
| `wakeup_events` / `ewic_cfg` | `WE_LPTIMER0` / `EWIC_VBAT_TIMER` (RTC: `WE_LPGPIO0` / `EWIC_VBAT_GPIO`) | `WE_LPTIMER0` / `EWIC_VBAT_TIMER` (or `WE_LPRTC` / `EWIC_RTC_A`) | same families |
| `vtor_address` | `SCB->VTOR` | **`SCB->VTOR`** (this image's own vector table) | `SCB->VTOR` |
| `vtor_address_ns` | `SCB->VTOR` | not set (uninitialised) | `SCB->VTOR` |

Process differences: the vendor sample **re-applies a full explicit RUN profile at
`PRE_KERNEL_1` priority 46 on every boot** (power domains `PD_SYST | PD_SSE700_AON`, DC-DC 825
mV PWM, `aon_clk_src` LFXO, `run_clk_src` PLL, 160 MHz, I/O flex 1.8 V, `memory_blocks`
`MRAM_MASK`), because after a SOFT_OFF wake the SoC comes up on whatever the SE left; this
backend does not (TODO addendum 6; bench U8c saw UART5 at ~1/5 baud and a slow tick, i.e. no
PLL). The vendor writes the OFF profile from a PM notifier and never reads it back or touches
`RET_CTRL` / `VBAT_ANA_REG1`. The EWIC entry (`pm_core_enter_deep_sleep_request_subsys_off`) is
the same sequence as ours, with `RTSS_HE_CTRL` written whole. The vendor wake timer is the same
LPTIMER0 (`timer0`, `snps,dw-timers`, IRQ 60) through the Zephyr counter API.

Bench U8g settled it: the two OFF fields that made the difference were `memory_blocks`
(MRAM \| SERAM) and `vtor_address` (`SCB->VTOR`; a preserved value that may be 0 resumes at an
empty ITCM), and both are now the backend default. `aon_clk_src` and `stby_clk_freq` remain
bench knobs. The boot-time clock restore now exists (`ALP_SDK_POWER_ALIF_SE_RESTORE_CLOCKS`).

**Wake decode.** The record in BKRAM carries what was armed; on the cold boot the
LPTIMER status is read before its driver initialises, and the RV-3028 flags and
calendar in the I2C restore pass. `slept_ms` is the RV-3028 calendar delta (1 s
resolution).

**Not verified on silicon:** that the SE accepts the profile, that the EWIC entry
removes power (`RTSS_HE_CTRL.COLD_WAKEUP` is cleared and `WIC` set by
read-modify-write), that BKRAM retains with bit 21, the HE TCM bank sizes and
ITCM / DTCM split, STANDBY, and that the LPGPIO holds survive the SE's wake boot.
`examples/aen/aen-power-stop` is the bench for the first three.
