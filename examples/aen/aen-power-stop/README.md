# aen-power-stop

Bench proof of the Alif SE STOP backend ([#2784](https://github.com/alplabai/alp-sdk/issues/2784),
unit U7) on the **E1M-AEN803** (Alif Ensemble E8, M55-HE; an HP build is described below). STOP **passed on silicon** (U8h, 3 of 3 cycles); STANDBY is untested.
Built as an **MRAM image**: STOP wakes through a cold boot (SES -> ATOC -> this
image), so a RAM-run image would not come back.

> Back up the module's MRAM image before flashing this and have the restore
> procedure ready. If the entry sequence is wrong the Secure Enclave can keep the
> module until it is power-cycled.

See [`docs/aen-power-domains.md`](../../../docs/aen-power-domains.md) for the model.

## M55-HP (STOP bench-proven HP-only on the E1M-AEN803)

The same app builds for the HP core (`alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`,
`alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp`); `boards/` carries the matching overlay and a
config fragment that turns the boot-time clock restore on (its Kconfig default is HE-only; the AEN803
HP fragment is bench-proven, the AEN801 HP one is unmeasured).

E1M-AEN401 (Ensemble E4, `alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp`): builds with its own
overlay and config (`boards/alp_e1m_aen401_m55_hp_*`), **not bench-run by Alp Lab** (a customer
ran 14/14 RV-3028 countdown wakes with the same backend port). Known gaps on E4: no `bkram` node,
so the wake record is lost across the cold boot and the per-cycle wake-source / `record_valid`
checks FAIL; no `alp,som-power` node, so no on-module domain is quiesced; the clock restore is off.

Bench (2026-10-10, E1M-AEN803 2026W36-0001 on an E1M-EVK, HP-only ATOC so the HE was not booted; `examples/aen/aen-power-stop` built for `alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp`, linked in the HP slot0 at `0x802b0000`, Flow D via tan): 3/3 STOP cycles PASS at 400 MHz with `RESTORE_CLOCKS` on. Cycle 1 LPTIMER 500 ms `mode=4 wake_source=0x8`, cycle 2 RV-3028 countdown 3 s `mode=4 wake_source=0x1`, cycle 3 RV-3028 alarm `mode=4 wake_source=0x1`; `quiesced=restored=0x7f`; `SUMMARY cycles=3 pass=5 fail=0`. Run D (combined HE idle + HP this example): the HP woke from the LPTIMER but `STOP_MODE_STAT` stayed clear (raw `0x100`, the DC-DC stays up while the HE runs), so the record was discarded and cycle 1 repeated. The backend now also trusts this core's own `RTSS_HP_CTRL` WIC [9:8] = `0b01` as a STOP witness (`docs/aen-power-domains.md`); bench re-run: dual-core (HE idle Zephyr app + HP `aen-power-stop`) on E1M-AEN803 2026W36-0001 passes 3/3 (LPTIMER `wake_source=0x8`, countdown `0x1`, alarm `0x1`, all `mode=4`, `SUMMARY cycles=3 pass=5 fail=0`, boot diag w[16..19] = `00000100 00000100 00000333 00000000`), flashed as one combined ATOC (DEVICE + m55_he + m55_hp) written by tan's Flow D with the alplabai/tan-cli#1509 fix; the HE-only regression stays 3/3. Still open: the AEN801 HP.

What differs from the HE, and what the app gets when it asks for it:

| Request | HP answer |
|---|---|
| `STOP` | the vendor's SOFT_OFF-class sleep; the wake is a cold boot, as on the HE |
| `STANDBY` | `ALP_ERR_NOSUPPORT` from the dispatcher before the backend runs (no wake source is valid for it on the HP; the S2RAM sleep is HE-only). The backend's own `reason=hp_standby_unsupported` check is defence in depth and is not reachable through `alp_power_request_sleep()` |
| TCM / FULL retention | `ALP_ERR_NOSUPPORT`, `reason=hp_tcm_not_retainable` (no HP TCM retention) |
| image not in MRAM (`VTOR` below `0x80000000`) | `ALP_ERR_NOSUPPORT`, `reason=hp_vtor_not_mram`; do not use the Flow C ITCM retarget |

Flash it as the HP's own `slot0_partition` (from the board DTS). The HE core is left to whatever
else runs on the module: the HP sleep powers only the HP subsystem down, and the SoC reaches its STOP
state only when every subsystem has done the same, so the HE should be idle or off when measuring.
The BKRAM record, the LPTIMER0 wake timer and the RV-3028 are single-owner: run this app on one core.

## What it does

`alp_power_request_sleep(STOP)` does not return on this part, so `main()` runs again
on every wake. A counter in the Utility SRAM (BKRAM; a bench-only cell,
`CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH`) says which cycle it is.

| Cycle | Wake source armed | Expected `wake_source` |
|---|---|---|
| 1 | LPTIMER, 500 ms | `ALP_POWER_WAKE_TIMER` |
| 2 | RV-3028 countdown, 3 s | `ALP_POWER_WAKE_RTC` |
| 3 | RV-3028 alarm, next minute change (INT -> P15_0) | `ALP_POWER_WAKE_RTC` |

Every cycle stays awake 10 s first, so a console can attach. After each wake it
judges the cycle that just ended from `alp_power_boot_wake_info()`, then starts the
next. After the third wake it prints the summary and stays awake.

A timed wake under 1 s uses the LPTIMER, whose 32 kHz source is the low-frequency
clock the SES leaves on the ring oscillator (about 4.5 % fast); from 1 s up the
RV-3028 countdown is used, because the RV-3028 is the trusted time base.

## Build and run

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-power-stop -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

Flash it to MRAM (Flow D, slot0 `he_slot0` at `0x80010000`). **The J-Link leaves
`DHCSR.C_DEBUGEN` set after it detaches, and firmware cannot clear it**: the backend
then refuses to sleep with `ALP_ERR_BUSY`. Clear it from the debugger BEFORE releasing
the core (`w4 0xE000EDF0 0xA05F0000`), then detach the J-Link, then issue the
flash-loader nRESET; or power-cycle the module with the probe detached. Read the
console on the E1M edge UART0 at 115200 8N1. Start from a **cold power cycle**: the
counter lives in SRAM that a reset keeps and a power cycle loses.

A refused sleep prints `alif_se_power: refuse step=<n> reason=<...> rc=<raw>` (and
`som_power: ...` for a pad or domain) before the `request FAIL` line.

> **No J-Link connect-under-reset and no `tan flash --readback` during the sleep window.**
> Either one asserts NSRST, which resets the module mid-sleep and looks like a failed wake
> (it is reported as an aborted sleep, not a wake, but the cycle is lost). Attach only
> before the first cycle or after the last one.

## Bench variants (#2784 addendum 6)

One variable each, selected with a config fragment on top of `prj.conf`
(`-DEXTRA_CONF_FILE=variants/<name>.conf`). All of them print the same evidence.

| Variant | Fragment | Changes | Read |
|---|---|---|---|
| (i) default, instrumented | `i-default.conf` | nothing | the baseline: `diag pre` / `diag boot` / `se run` / `se off` |
| (ii) RV-3028 first | `ii-rtc-first.conf` | cycle order: countdown, LPTIMER, alarm | if the countdown cycle wakes and the LPTIMER one does not, the INT path is sound and the LPTIMER path is the suspect |
| (iii) LPTIMER 5 s | `iii-lptimer-5s.conf` | LPTIMER interval 5000 ms (the backend bench option raises the LPTIMER ceiling to 10 s; the RV-3028 countdown cycle moves to 11 s so it stays on the RV-3028) | whether a longer interval changes the outcome (a race with the SE calls, or the clock) |
| (A) = (iii) | `iii-lptimer-5s.conf` | the 5 s LPTIMER cycle (the U8d plan's "A"; the OFF profile is now the vendor one by default) | |
| (B) = (i) | `i-default.conf` | LPTIMER 500 ms | the baseline |
| (iv) LFXO | `iv-lfxo.conf` | OFF profile `aon_clk_src` = LFXO (cap 63) | the vendor sample's choice; compare the wake and `se off aon_clk` |
| (P) product | `p-product.conf` | vendor OFF (default) + fixed restore, LPTIMER 5 s, then countdown, then alarm | a wake at 115200 baud, `bkram live=1 selftest=1` |
| (P-novtor) | `p-novtor.conf` | (P) with the live vtor_address | ablation of the resume vector |
| (P-nomem) | `p-nomem.conf` | (P) without MRAM \| SERAM in memory_blocks | ablation of the memory blocks |
| (P-500) | `p-500.conf` | (P) with LPTIMER 500 ms | the short interval |
| (product-noscratch) | `product-noscratch.conf` | the shipping configuration: no bench scratch option (no BKRAM bench cell, diag, image CRC, LPRTC witness or `RTC_CLK_EN` write), default 1 s LPTIMER ceiling, restore on; the cycle number is kept in the RV-3028 User RAM 1 | cycles LPTIMER 500 ms, countdown 3 s, alarm; prints `wake_source`, `slept_ms`, `clocks uart_baud=... tick_ms_per_2_rtc_s=...` and the verdicts (`tick_rate` instead of `bkram_counter`) |
| (A-norestore) | `vii-a-norestore.conf` | (P) with `RESTORE_CLOCKS=n` | no boot-time `set_run_cfg`; the UART comes up at 1/5 baud |
| (R-vendor) | `ix-repro-vendor.conf` | forces the boot-time restore on a cold boot (fixed profile), then BKRAM self-test, then cycle 1 in the same boot | `bkram live=1 selftest=1` and BOOT w40 = 1, w52 = 1 |
| (R-legacy) | `x-repro-legacy.conf` | the same with the c6de654ff restore profile (0x16d, no BACKUP4K, no re-assert) | reproduces the U8e loss: `bkram live=0`, BOOT w52 = 2, the sleep refused `bkram_unusable` |

### STANDBY, TCM retention and wake-to-main() timing

Further fragments. Bench results on an E1M-AEN803 (2026W36-0001) on an E1M-EVK: `s-standby` and
`t-tcm-retain` ran 3/3 cycles with every verdict PASS (2026-10-10); `w-wake-timing` measured
wake-to-`main()` = 92.6 ms three times within 61 us (2026-10-11, see the timing note below); `h-hp-running`
ran 3/3 cycles with every verdict PASS (2026-10-11). `t-tcm-retain-128k` was not run. Each is selected like
the others (`-DEXTRA_CONF_FILE=variants/<name>.conf`); the default image is unchanged.

| Variant | Fragment | Changes | Expected evidence |
|---|---|---|---|
| (S) STANDBY | `s-standby.conf` | every cycle requests `ALP_POWER_MODE_STANDBY` | `cycle<n> enter STANDBY (...)`, then per cycle `cycle<n> mode_standby PASS` (the wake record's `realised_mode` reads STANDBY) next to the same `record_valid`, `wake_source`, `restored_all`, `bkram_counter` checks. A `request FAIL rc=... realised=...` line, or `valid=0` after the wake, means the SE did not take the STANDBY profile or the record did not survive. |
| (T) TCM, both banks | `t-tcm-retain.conf` | `ALP_POWER_RETAIN_TCM`, `retain_kb = 256` (DTCM SRAM5_1 + SRAM5_2), plus a pattern probe | before each sleep `POWER_STOP: tcm cycle<n> wrote retain_kb=256 addr=0x... bank0_bytes=... bank1_bytes=... crc_a=0x... crc_b=0x...`; after the wake `tcm cycle<n> read asked_banks=2 bank0=retained bank1=retained` and `cycle<n> tcm_retained PASS` |
| (T-128) TCM, one bank | `t-tcm-retain-128k.conf` | `retain_kb = 128` (SRAM5_1 requested) | `asked_banks=1`; the verdict covers bank 0. `bank1=` is NOT mapping evidence: with any TCM asked for the backend ORs both `RET_CTRL.HETCM1 \| HETCM2` after the SE call (the `tcm_requested()` branch of the retention setup in `alif_se_power.c`), so both DTCM halves stay powered. Mapping evidence needs a backend change that sets only the matching bit (to be tracked) |
| (W) timing | `w-wake-timing.conf` | samples the LPRTC (counter and prescaler sub-count) at sleep entry, at `PRE_KERNEL_1` and at `main()`, calibrates its clock against two RV-3028 seconds, and replaces the bench image CRC with a constant (see the note) | `POWER_STOP: timing cycle<n> lprtc cpsr=... hz=... backwards=0 pre=<ccvr>/<cpcvr> boot=... main=... main_uptime_ms=...`, then `entry_to_main_us=... boot_to_main_us=... rtc_slept_ms=...`; for the LPTIMER cycle also `lptimer load=... timer_clksel=0x0 wake_to_boot_us=... wake_to_main_us=... (+-<n> us)`, for the RV-3028 cycles `wake_to_main_us=n/a`. Measured: `hz=34257`, `wake_to_boot_us=5108`..`5166`, `wake_to_main_us=92562`..`92623` (+-59 us). No verdict. |
| (H) HP running | `h-hp-running.conf` | flashed with `examples/aen/aen-hp-core-smoke` on the M55-HP in one ATOC (`tan flash`, one system manifest naming both cores; the smoke built MRAM-linked with an empty `DTC_OVERLAY_FILE`); sets `CONFIG_ALP_SDK_POWER_ALIF_SE_OFF_KEEP_MEMORY=0x3` | after each wake `POWER_STOP: hp_beacon cycle<n> magic=0xa11fe000 heartbeat=... he_uptime_ms=... slept_ms=...` and `cycle<n> hp_kept_running PASS`: the HP's 100 ms heartbeat counted more than this boot plus the sleep, so the HP ran through the HE's STOP. Measured heartbeat 115, 251, 439. With the keep mask at 0 the same run reads `magic=0x00000000 heartbeat=0` and fails 3/3: the SE powers SRAM0 off when the HE's subsystem goes off, and the HP faults on its next SRAM0 store |
| (G) wake pad | `g-wake-pad.overlay` (a devicetree overlay, selected with `-DEXTRA_DTC_OVERLAY_FILE=variants/g-wake-pad.overlay`, not `EXTRA_CONF_FILE`) | an `alp,power-wake-gpios` node naming P15_2, so the wake-pad branch of the STOP backend (and its `PRE_KERNEL_1` pad snapshot hook) is compiled | Bench-run on the E1M-AEN803 2026W36-0001 (2026-10-10): exactly as expected, STOP 3/3. At boot `POWER_STOP: wake_pads requested=0x4 claimed=0x4 advertised=0` (P15_2 is the SoM's OSPI INTn net, so the pad is refused and `ALP_POWER_WAKE_GPIO` is not advertised; the default image prints `requested=0x0 claimed=0x0 advertised=0`), then the same three STOP cycles and `wake_source` verdicts as the default image. `requested=0x4 ... advertised=1` would mean the SoM no longer claims the pad. |

**How the TCM probe works.** The whole of Zephyr's RAM is the 256 KiB M55-HE DTCM (CPU-local
`0x20000000`). The probe is a 160 KiB `.noinit` array, so the C runtime neither copies nor zeroes it
on the cold boot; it straddles the 128 KiB boundary wherever the linker puts it, so part A sits in
the first bank and part B in the second. A different pattern is written each cycle (seeded by the
cycle number) and checked by CRC-32 after the wake, with the expected CRC regenerated rather than
stored. The bank size (two 128 KiB DTCM banks) is taken from hal_alif `se_services/include/aipm.h`
(`MB_SRAM5_1`, `MB_SRAM5_2`, "dtcm 128kb") and the E8 DFP `SOC_FEAT_HE_DTCM_SIZE`. **Still
unproven:** that `SRAM5_1` is the low half of the address range, and that the SRAM5_x bits drive
`RET_CTRL.HETCM_RET1/2` (this example cannot show it: both RET_CTRL bits are always set). A `lost` on a bank that was asked for is a real finding (the SES may also
scrub the TCM on the cold boot), not necessarily a test fault; read `bank0=`/`bank1=` together with
`asked_banks=`.

**How the timing is derived, and how good it is.** The LPRTC (VBAT domain, `0x42000000`) keeps running
through STOP and the SE cold boot. Its counter `CCVR` ticks once per `CPSR` LPRTC clocks (16926 here,
about 2 Hz), and `CPCVR` (+0x24) is the prescaler's count inside the current tick, so
`CCVR * CPSR + CPCVR` counts single LPRTC clocks (about 29 us). The backend samples that pair, re-read
until `CCVR` did not tick between the two reads, into the `PRE` snapshot (right after the LPTIMER is
armed, interrupts off) and the `BOOT` snapshot (`PRE_KERNEL_1` priority 0); `main()` samples it first
thing. The LPRTC runs on the SES-left LFRC, whose rate is not pinned, so after the wake the example
measures it against two RV-3028 seconds (+-1 ppm): 34252 to 34268 Hz on the bench. `backwards=0` says
the clock count never went down over that measurement, i.e. the counter model holds on this part.
The wake instant is known only for the LPTIMER cycle: the timer is armed `LOADCOUNT` (PRE word 3)
before the snapshot, minus the arm-to-snapshot gap (DWT, PRE word 13), on the same 32 kHz clock
(`timer_clksel=0x0`), so `wake_to_main_us` is a measurement to +-2 LPRTC clocks. The RV-3028 cycles
wake on a 1 s timer and print only entry-to-main.

What the 92.6 ms is made of (temporary LPRTC probes, 2026-10-11; each probe read costs about 1 ms, so
the split is approximate): about 5 ms from the wake to Zephyr's first `PRE_KERNEL_1` hook (the SE cold
boot), about 71 ms in the SoM domain restore's first pass (`POST_KERNEL` 0), and the remaining ~16 ms
for the clock check, the I2C pass and the rest of init. The 71 ms are the restore's datasheet
busy-waits (Ethernet PHY power-up, reset and settle; the memory resets; the Wi-Fi rail) for the
domains this example quiesces, which is all of them; an application that quiesces fewer waits less.
`main_uptime_ms` reads 85 because the
kernel's clock starts at `PRE_KERNEL_2`. The bench image identity (a CRC32 over the whole ROM) runs at
`PRE_KERNEL_1` on the SE-left clocks and costs 197 ms there; a product build does not have it, and this
variant overrides it with a constant so the figure is the product's boot. That override means a reflash
of another build keeps this one's BKRAM counter: start each run from a power cycle. It needs the bench
scratch option (on in `prj.conf`) for the `PRE` / `BOOT` snapshots, so it does not exist in the
`product-noscratch` image.

Product OFF profile (bench U8g: STOP woke 2 of 2 with it, never without): `vtor_address` =
`SCB->VTOR` and `memory_blocks` = MRAM \| SERAM \| BKRAM are the backend default. The bench
build can turn each off (`alp_som_bench_knobs`, Kconfig `..._BENCH_NO_VTOR_SELF` /
`..._BENCH_NO_MRAM_SERAM`, plus `..._BENCH_FORCE_LFXO` / `..._BENCH_STBY_76_8` for the other two
vendor differences); the knobs are fixed per image because the Zephyr shell owns the console RX.
Every boot prints `POWER_STOP: knobs ...`, `POWER_STOP: ses revision ...` (SE firmware revision
and TOC version) and `POWER_STOP: bkram live=<0|1> selftest=<0|1>`; the BOOT diag words 52-55
hold the restore's BKRAM self-test result (1 ok, 2 failed), RET_CTRL and VBAT_ANA_REG1 before the
re-assert, and VBAT_ANA_REG1 after it.

See `docs/aen-power-domains.md` for the comparison with the vendor `system_off` sample.

## What to read after a wake

Each boot prints, in order: `stop_mode_reg=...`, `diag pre` (the registers immediately
before the WFI of the previous sleep, from BKRAM), `diag boot` (the registers at
`PRE_KERNEL_1` of THIS boot, before any restore or driver), `se run` and `se off` (the SE's
profiles as they stand). The word index of the diag blocks is listed in
`src/backends/power/alif_se_power_hw.c`. The questions they answer:

- **Did the LPTIMER count and fire before the WFI?** `pre` words 0 (CONTROLREG: bit 0 enable,
  bit 2 interrupt mask), 1/2 (RAWINT/INTSTATUS), 3 (LOADCOUNT), 4 (CURRENTVAL), 13 (CYCCNT
  between arm and snapshot) and 11/12 (NVIC ISER1/ISPR1, IRQ 60 = bit 28).
- **Was the wake source reaching the SE?** `pre` word 5 (`WKUP_CTRL`: LPTIMER bits [11:8]).
- **What did the clock tree look like after the wake?** `boot` words 20-27 (CGU `OSC_CTRL`,
  `PLL_LOCK_CTRL`, `PLL_CLK_SEL`, `ESCLK_SEL`, `CLK_ENA`, `ACLK_CTRL`, `SYSTOP_CLK_DIV`,
  `UART_CTRL`) against the cold-boot values, and `se run` against the cold-boot profile.
- **Was it a wake or a reset?** `boot` word 19 (`RTSS_HE_RESET`: 0 SE-initiated, 1 NSRST
  pin, 4 power-domain request) and `boot` word 16 (`STOP_MODE_STAT`, bit 4). A pin reset
  is reported as an aborted sleep (`valid=1 mode=0 wake_source=0x0`), not a wake -- except
  after a sleep that retains TCM: its wake also leaves `RTSS_HE_RESET` = 1, so such a record
  never trusts the bit, and a real pin reset during a TCM-retaining sleep reads as a wake.

## Bench contract

```
POWER_STOP: cycle<n> <check> <PASS|FAIL>
POWER_STOP: SUMMARY cycles=<n> pass=<n> fail=<n>
```

Checks, per cycle: `record_valid mode_stop wake_source restored_all bkram_counter`
(`mode_standby` replaces `mode_stop` in the STANDBY variant, and `tcm_retained` is added by the
TCM variants).
Evidence lines (no verdict): `POWER_STOP: boot`, `wake`, `regs` (RET_CTRL,
VBAT_ANA_REG1, MISC_CTRL, STOP_MODE, RTSS_HE_CTRL), `alarm`.

Acceptance (design U8): the counter reads 1, 2, 3 across the wakes; `STOP_MODE_STAT`
(bit 4 of `STOP_MODE`) is set at each wake; `wake_source` matches the armed source;
`quiesced` equals `restored`; no power cycle in between.

## Hygiene added after bench U8d

- **Image identity.** The record and the counter carry a CRC-32 of the running image's ROM
  region. A clean flash of another image finds a foreign counter or record: it starts fresh
  (counter 0, record cleared, PRE diag invalidated) and ignores `STOP_MODE_STAT` for that boot,
  so a reset taken after a previous run's sleep is never read as this image's wake.
- **`STOP_MODE_STAT` is acknowledged** (a write of exactly `0x00000010`, never bit 0 -- bit 0
  is `STOP_MODE_CTRL` and enters stop mode) once the boot decode is done. Diag word 50 holds the
  read-back.
- **Clock restore.** If the PLL is not locked or the PLL clock select is not the running tree,
  the backend re-applies the full RUN profile at `PRE_KERNEL_1` (priority 46). Diag words 40-46
  hold what happened (40: 0 healthy, 1 restored, 2 failed; 41: the SE return code; 42-45: PLL
  lock, PLL clock select, OSC and ACLK after; 46: the RUN `memory_blocks` the SE reports).
- **Wake attribution is time-gated.** An LPTIMER wake is rejected when the RV-3028 (or LPRTC)
  shows far more time than the armed interval plus a boot passed: the periodic LPTIMER stays
  pending after any reset.

