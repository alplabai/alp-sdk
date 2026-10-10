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
STANDBY)` on the E8 M55-HE (E1M-AEN801 / E1M-AEN803) and `STOP` on the E8 M55-HP (**bench-proven HP-only** on the E1M-AEN803) (section "The M55-HP" below). It is registered for
`alif:ensemble:e8` behind `CONFIG_ALP_SDK_POWER_ALIF_SE` (default **n**,
experimental; STOP is bench-proven on the E1M-AEN803, STANDBY is not). This core's (HE or HP) subsystem is powered off and the
wake is a cold boot through the Secure Enclave, so the call does not return: read
the cause with `alp_power_boot_wake_info()`. `SLEEP` / `DEEP_SLEEP` are forwarded to
the pm_policy backend when it is built.

**Measured behaviour (STOP, bench U8h, E1M-AEN803 on an E1M-EVK, head 680aaebd3).** Three
STOP cycles in one image all woke, every verdict passed: the LPTIMER at 5 s (`slept_ms` 6000)
and 500 ms (`slept_ms` 1000), the RV-3028 countdown at 11 s and the RV-3028 alarm. Intervals
from 500 ms to 11 s were exercised. Every wake boot came up at 115200 baud with the boot-time
clock restore having run (`RESTORE_CLOCKS`, BOOT diag w40 = 1) and BKRAM live. Without that
restore a wake comes up at 23040 baud with a half-rate tick. Supply current at the EVK
board's 16 V input (a DPS reading of the whole EVK, **not** a SoC measurement): about
0.043 A in STOP against 0.058 A awake. **`memory_blocks` MRAM | SERAM | BKRAM is required:**
with MRAM | SERAM left out the board still returned on time, but the SE rebooted through the
cold path (BOOT diag w19 = 1, `RET_CTRL` 0x0002aaf0, the OFF profile reads back cleared). The
boot took the trusted-NSRST external-reset path: the reset syndrome showed NSRST (bit 0, probed
before the sleep and recorded as trustworthy) and `STOP_MODE_STAT` agreed with the record, so the
return is reported as an aborted sleep (`valid`, `realised_mode` RUN, no wake source) with the
domains still restored. That is the correct classification, and a build can not select the
memory-less profile outside the bench scratch option; the backend also checks the profile it
built and refuses to send one without MRAM | SERAM.
`vtor_address` = `SCB->VTOR` is optional (3 of 3 without it) and stays the default, as in the
vendor sample.

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

### The M55-HP (STOP bench-proven HP-only on the E1M-AEN803)

The backend compiles and registers for the HP core too (`CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP`,
board targets `alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp` and
`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`; `examples/aen/aen-power-stop` builds for both).
**Bench (2026-10-10, E1M-AEN803 2026W36-0001 on an E1M-EVK, HP-only ATOC, i.e. the HE was not booted):**
`examples/aen/aen-power-stop` built for `alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`, linked in HP
slot0 at `0x802b0000`, Flow D via tan. 3/3 STOP cycles PASS at 400 MHz with `RESTORE_CLOCKS` on: cycle 1
LPTIMER 500 ms `mode=4 wake_source=0x8`, cycle 2 RV-3028 countdown 3 s `mode=4 wake_source=0x1`,
cycle 3 RV-3028 alarm `mode=4 wake_source=0x1`, `quiesced=restored=0x7f`,
`SUMMARY cycles=3 pass=5 fail=0`. Not verified: an HP STOP while the HE runs (needs a combined HE+HP
ATOC the current tooling cannot build) and the E1M-AEN801 HP. The differences in the table below are
transcribed from the Alif sources named in the code comments.

| Aspect | M55-HE (bench-proven STOP) | M55-HP (STOP bench-proven, AEN803) | Source |
|---|---|---|---|
| Sleep modes | `STOP`, `STANDBY` | `STOP` only (the vendor's SOFT_OFF-class profile); `STANDBY`: the backend's per-mode wake caps for it are empty, so the dispatcher refuses it with `ALP_ERR_NOSUPPORT` before the backend runs (the backend's `hp_standby_unsupported` check is defence in depth) | sdk-alif `samples/drivers/pm/system_off/src/main.c:89,305`, README ("S2RAM ... HE core only") |
| TCM retention | HE TCM banks (`SRAM4_x` / `SRAM5_x`) | none: `ALP_POWER_RETAIN_TCM` / `FULL` are `ALP_ERR_NOSUPPORT` (`hp_tcm_not_retainable`); `memory_blocks` is BKRAM \| MRAM \| SERAM only | `main.c:199-201`; hal_alif `aipm.h:195-259` has no HP TCM block |
| Boot location | any (`vtor_address` optional, U8h) | MRAM only: `SCB->VTOR` below `0x80000000` is refused `ALP_ERR_NOSUPPORT` (`hp_vtor_not_mram`) | `main.c:80,85,199-201` |
| Core control / reset registers | `AON.RTSS_HE_CTRL` `0x1A604010`, `RTSS_HE_RESET` `0x1A604014` | `AON.RTSS_HP_CTRL` `0x1A604000`, `RTSS_HP_RESET` `0x1A604004` (same bits: `COLD_WAKEUP` [0], `WIC` [9:8], `RESETSYNDROME` [5:0]) | DFP `Device/soc/AE822FA0E5597/include/rtss_hp/soc.h:1157-1158,3656`; `Device/core/common/source/pm.c:61-69`; HP SVD `:3890,3913` |
| Entry sequence | `pm_core_enter_deep_sleep_request_subsys_off()` step for step | the same, on the HP's own register pair | DFP `pm.c` (one function, `#if defined(RTSS_HP)` picks the registers) |
| Wake events, EWIC | `WE_LPTIMER0` / `EWIC_VBAT_TIMER`, `WE_LPGPIO0` / `EWIC_VBAT_GPIO` | identical: SoC-level `off_profile_t` fields, and IRQ 57 (`LPGPIO_COMB_IRQ_IRQn`) and 60 (`LPTIMER0_IRQ_IRQn`) are the same lines in the HP header | `rtss_hp/soc.h:140,143`; `main.c:45-50` has no HP branch |
| Power domains | `PD_VBAT_AON` (STOP) | `PD_VBAT_AON` (the HP is not in the list, so its subsystem is the one that powers off) | `main.c:214-215`; hal_alif `aipm.h:56,68` (`PD_RTSS_HP` = bit 7) |
| BKRAM wake record | `0x4902C000`, retained | same SoC-level block, same record | `ensemble_e8_peripherals.dtsi` `bkram` node |
| Clock restore (`..._RESTORE_CLOCKS`, default **off** for the HP boards, on in the example's HP fragment; measured on the AEN803 HP, unmeasured on the AEN801 HP) | RUN profile 160 MHz; healthy = `PLL_CLK_SEL` `ES1` [20] \| `SYS` [4] \| `SYSREF` [0] (`0x00100011`) | RUN profile 400 MHz; healthy = `ES0` [16] \| `SYS` [4] \| `SYSREF` [0] (`0x00010011`) | `main.c:137-138`; HP SVD `PLL_CLK_SEL` `:1862` |

**What the backend does about the other core.** The OFF profile belongs to the calling core. The SE
powers that core's subsystem off, and the SoC drops to its STOP state only once every subsystem has
configured and entered its own sleep (DFP `pm.c`, header comment of
`pm_core_enter_deep_sleep_request_subsys_off()`). The backend cannot see the other core, and no
register in the DFP or hal_alif says whether it is up, so it neither waits for it nor refuses on
it. With the HE running while the HP sleeps, only the HP subsystem goes down: the SoC stays in
RUN, the HP still wakes by a cold boot, and the on-module domains the HP quiesced stay held until it is
back, so the HE must not be using them. For a real SoC-level STOP both cores have to be in their own
sleep, each with its own OFF profile. The BKRAM record, the LPTIMER0 wake timer and the RV-3028 are
single-owner resources: use the backend from one core only (the dtsi notes the same for the
LPTIMER; the vendor README says the RTC is shared with the HE).

**Open on the HP (bench):** an HP STOP while the HE runs. `STOP_MODE_STAT` (a SoC-level flag,
`VBAT_STOP_MODE_REG`) is probably not set when only the HP powers off, and the record cross-check
depends on it, so such a wake could decode as an aborted sleep (the domains are still restored). Also
unmeasured: the E1M-AEN801 HP, and the HP/HE co-sleep case above. The HP-only AEN803 run above covers
the SE accepting the profile, the 400 MHz RUN profile, the clock-restore trigger and the LPTIMER0 and
RV-3028 wakes.

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
backend does the same at the same slot (`clock_restore`, `ALP_SDK_POWER_ALIF_SE_RESTORE_CLOCKS`),
but only when the hardware says the clock tree is not the running one (PLL not locked, or the
three `PLL_CLK_SEL` bits for this core wrong); bench U8c saw UART5 at ~1/5 baud and a slow tick
without it. The vendor writes the OFF profile from a PM notifier and never reads it back or touches
`RET_CTRL` / `VBAT_ANA_REG1`. The EWIC entry (`pm_core_enter_deep_sleep_request_subsys_off`) is
the same sequence as ours, with `RTSS_HE_CTRL` written whole. The vendor wake timer is the same
LPTIMER0 (`timer0`, `snps,dw-timers`, IRQ 60) through the Zephyr counter API.

Bench U8g / U8h settled the OFF fields: `memory_blocks` MRAM \| SERAM \| BKRAM is required (without
it the SE reboots instead of resuming), `vtor_address` = `SCB->VTOR` is optional (3 of 3 cycles
without it) and stays the default as in the vendor sample. `aon_clk_src` (LFXO) and
`stby_clk_freq` (76.8 MHz) remain bench-only knobs; STOP works without them.

**Wake decode.** The record in BKRAM carries what was armed; on the cold boot the
LPTIMER status is read before its driver initialises, and the RV-3028 flags and
calendar in the I2C restore pass. `slept_ms` is the RV-3028 calendar delta (1 s
resolution).

**Verified on silicon (bench U8h, E1M-AEN803):** the SE accepts the profile, the EWIC entry
removes power and the wake is a cold boot, BKRAM (bit 21) retains the record across STOP, and
the LPTIMER (500 ms, 5 s) and the RV-3028 countdown (3 s, 11 s) and alarm wake the SoC.
**Not verified:** STANDBY, the HE TCM bank sizes and ITCM / DTCM split, retention of
application RAM, that the LPGPIO holds survive the SE's wake boot on every board population,
the E1M-AEN801, and the E4. `examples/aen/aen-power-stop` is the bench (the
`product-noscratch` variant is the shipping configuration without the bench cell).
