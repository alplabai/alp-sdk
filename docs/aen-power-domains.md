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
experimental; STOP and STANDBY are bench-proven on the E1M-AEN803). The M55-HE subsystem is powered off and the
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
| `ALP_POWER_WAKE_GPIO` | application LPGPIO pads named by an `alp,power-wake-gpios` devicetree node, `WE_LPGPIO<n>` | advertised only when the node exists and none of its pads is wired by the SoM (see "LPGPIO wake pads" below). |
| (timed wake >= 1 s) | RV-3028 countdown (`rv3028c7_timer_start`) | whole seconds, rounded up. Needs the chip context bound (`alp_som_power_bind_rv3028`). |

A timed wake reports the source the caller asked for, whichever hardware serves it:
`ALP_POWER_WAKE_TIMER` (also when only `wake_after_ms` was given), or
`ALP_POWER_WAKE_RTC` when only `ALP_POWER_WAKE_RTC` was configured. The RV-3028
`INT` pad is armed as a falling-edge interrupt, inside the interrupt-off entry
section, so the edge is latched even by a short pulse (the part's pulse mode,
tRTN1 = 7.8 ms, is never enabled by the SDK: `INT` stays low until the wake decode
clears the flag). The LPGPIO combined line (IRQ 57) is the one that reaches the
EWIC; its handler only masks the line again.

**LPGPIO wake pads (`ALP_POWER_WAKE_GPIO`).** The application names its pads in the devicetree, so
`<alp/power.h>` gains no symbol:

```dts
wake-pads {
	compatible = "alp,power-wake-gpios";
	wake-gpios = <&lpgpio N GPIO_ACTIVE_LOW>;   /* P15_N: a line the variant frees (see "Refused pads") */
	pinctrl-0 = <&pinctrl_wake_pads>;           /* LPGPIO function, input buffer, pull */
	pinctrl-names = "default";
};
```

- *Which hardware.* Only the LPGPIO island (P15_0..P15_7) is in the VBAT domain that stays powered
  through STOP. Line n is wake event `WE_LPGPIO<n>` (bit 16 + n of `off_profile_t.wakeup_events`,
  hal_alif `se_services/include/aipm.h:320-327`; the Alif DFP `demo_pm.c:751` pairs P15_4 with
  `WE_LPGPIO4`) and the whole group reaches the EWIC through `EWIC_VBAT_GPIO` (`aipm.h:353`).
  Those wake-event bits are the ones `ANA.WKUP_CTRL.LPGPIO` [23:16] carries (`alif_aipm_gen2.h`); the backend does not write that register.
- *Polarity and edge.* Each entry is armed as an edge TO its asserted level (`GPIO_ACTIVE_LOW`:
  falling, `GPIO_ACTIVE_HIGH`: rising), the way the vendor demo arms its joyswitch pad
  (`demo_pm.c:359-361`). The pad must be idle when the request is made: a pad that already reads
  asserted once the edge is armed is `ALP_ERR_BUSY`, as for the RV-3028 `INT`.
- *Which pad fired.* The DW GPIO block latches the edge in `GPIO_RAW_INTSTATUS` (LPGPIO base
  `0x42002000` + `0x44`, DFP `soc.h:1575`) until `GPIO_PORTA_EOI` (+ `0x4C`) is written. The BKRAM
  record keeps the armed pads in `armed_hw` bits 23:16. `gpio_dw`'s init (PRE_KERNEL_1) writes
  `INTEN = 0` and `PORTA_EOI = ~0` (Zephyr `drivers/gpio/gpio_dw.c:460-461`), which would erase the
  latch before the POST_KERNEL wake decode runs, so a PRE_KERNEL_1 priority-0 hook snapshots
  `GPIO_RAW_INTSTATUS` first and the decode reads that snapshot, sets `ALP_POWER_WAKE_GPIO` in
  `wake_source` and has nothing left to acknowledge. An aborted sleep reads the live latch right
  after the `WFI` and acknowledges it. `alp_power_boot_wake_info()` reports only the bit; an
  application with several pads reads its own pads' levels.
- *Refused pads.* A pad the SoM wires is never a wake pad. `alp_som_power_lpgpio_claimed()` is every
  LPGPIO pad of an `alp,som-power-domain` node (P15_0 RV-3028 `INT`, P15_1 `E_WIFI_NRST`, P15_4
  `E_PHY_PWRDWN`, P15_5 `WIFI_EN`, and on the AEN803 P15_6 `OSPI0_RESETn`, P15_7 `OSPI1_RESETn`)
  plus the pads in the `alp,wired-lpgpio-pads` property of the `alp,som-power` node. The generated
  board dts of every AEN SoM lists P15_2, P15_3, P15_6 and P15_7 there (the OSPI `INTn` / `RESETn`
  nets, `alif-ospi.tsv`, wired to the memory footprints on the one E1M-AEN-2626-R2 PCB whether or
  not the part is populated); the source is `wired_lpgpio_pads:` in `on-module-links.yaml`. One claimed pad in the node makes `ALP_POWER_WAKE_GPIO` unadvertised
  and `alp_power_configure_wake_source()` answers `ALP_ERR_NOSUPPORT`.
  **On the E1M-AEN801 / E1M-AEN803 R2 all eight lines are SoM-wired, so no pad is accepted on the
  bare module**; the path is for a variant or a derivative where a line is freed.
- *Not done.* The DW debounce filter (`GPIO_DEBOUNCE`, `gpio_enable_debounce()` in the DFP
  `drivers/include/gpio.h:351`) needs its clock (`GPIO_DB_CKEN`, `RTSS_HE_LPPERI_CKEN` [9:8]) to keep
  running through the SE's STOP profile; nothing in the DFP states that, so it is left off.
  [BENCH] that the latch survives the SES boot, and that `EWIC_VBAT_GPIO` wakes on the pad edge,
  are unverified: only the P15_0 path has run on silicon.

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
**Verified on silicon (E1M-AEN803 2026W36-0001 on an E1M-EVK, 2026-10-10):** STANDBY
(`s-standby`: the LPTIMER, countdown and alarm cycles all woke with `realised_mode` STANDBY) and
DTCM retention across STOP (`t-tcm-retain`, `retain_kb` 256: a CRC-checked pattern in both
DTCM halves survived all three cycles). A TCM-retained STOP wake leaves `RTSS_HE_RESET` = 0x01,
the value the SVD names "NSRST pin asserted" and also the value a cold power-on leaves (a STOP
wake without TCM leaves 0x10), so a sleep that retains TCM does not mark its record
`ALP_SOM_REC_NSRST_TRUSTED` and its wake is never read as a pin reset. `w-wake-timing` ran but
cannot resolve wake-to-`main()`: the LPRTC ticks at about 2 Hz and the RV-3028 at 1 s, so the
only measured figure is Zephyr start to `main()` = 85 ms (`main_uptime_ms`); the SE boot before
it needs a GPIO edge on a scope or a SoM current trace.
**Not verified:** the HE TCM bank sizes and the ITCM / DTCM split (both DTCM halves stay
powered whenever any TCM is asked for), retention of application RAM, the LPGPIO wake pads (`ALP_POWER_WAKE_GPIO`: no pad is free on
the E1M-AEN801/803 R2, so it is never advertised there. On the E1M-AEN803 2026W36-0001 the
`g-wake-pad` variant, which compiles the pad branch and its `PRE_KERNEL_1` snapshot hook, refused
P15_2 as SoM-claimed and ran STOP 3/3; a wake through a pad has never run, because no pad is
free), that the LPGPIO holds
survive the SE's wake boot on every board population, the E1M-AEN801, and the E4. The
`product-noscratch` variant of `examples/aen/aen-power-stop` is the shipping configuration without
the bench cell.
