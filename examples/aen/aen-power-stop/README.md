# aen-power-stop

Bench proof of the Alif SE STOP backend ([#2784](https://github.com/alplabai/alp-sdk/issues/2784),
unit U7) on the **E1M-AEN803** (Alif Ensemble E8, M55-HE). **Untested on silicon.**
Built as an **MRAM image**: STOP wakes through a cold boot (SES -> ATOC -> this
image), so a RAM-run image would not come back.

> Back up the module's MRAM image before flashing this and have the restore
> procedure ready. If the entry sequence is wrong the Secure Enclave can keep the
> module until it is power-cycled.

See [`docs/aen-power-domains.md`](../../../docs/aen-power-domains.md) for the model.

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
| (A) = (iii) | `iii-lptimer-5s.conf` | the 5 s LPTIMER cycle with the live vtor (the U8d plan's "A") | |
| (A-vtor) | `a-vtor-lptimer-5s.conf` | (A) plus `vtor_address` = own VTOR | whether the vendor resume vector changes the outcome |
| (B) = (i) | `i-default.conf` | LPTIMER 500 ms | the baseline |
| (iv) LFXO | `iv-lfxo.conf` | OFF profile `aon_clk_src` = LFXO (cap 63) | the vendor sample's choice; compare the wake and `se off aon_clk` |
| (v) VTOR self | `v-vtor-self.conf` | OFF profile `vtor_address` = this image's VTOR | the vendor sample's resume vector; the default keeps the live value |
| (vi) MRAM+SERAM | `vi-mram-seram.conf` | OFF profile `memory_blocks` also MRAM \| SERAM | the vendor sample's MRAM-boot profile |
| (A-norestore) | `vii-a-norestore.conf` | (A) with `RESTORE_CLOCKS=n` | the control for U8e: no boot-time `set_run_cfg` |
| (V) vendor OFF | `viii-vendor-off.conf` | (A) with vtor = own VTOR and memory MRAM \| SERAM \| BKRAM, `RESTORE_CLOCKS=n` | the vendor OFF profile; ablate at run time with the console keys below |
| (R-vendor) | `ix-repro-vendor.conf` | forces the boot-time restore on a cold boot (fixed profile), then BKRAM self-test, then cycle 1 in the same boot | `bkram live=1 selftest=1` and BOOT w40 = 1, w52 = 1 |
| (R-legacy) | `x-repro-legacy.conf` | the same with the c6de654ff restore profile (0x16d, no BACKUP4K, no re-assert) | reproduces the U8e loss: `bkram live=0`, BOOT w52 = 2, the sleep refused `bkram_unusable` |

Run-time knobs (bench build): during the 10 s awake window send `v` (OFF `vtor_address` =
`SCB->VTOR`), `m` (`memory_blocks` |= MRAM \| SERAM), `l` (`aon_clk_src` LFXO) or `s`
(`stby_clk_freq` 76.8 MHz) on the console; each toggles one knob and prints
`POWER_STOP: knobs ...`. They live in RAM, so each wake starts from the Kconfig defaults. Every
boot also prints `POWER_STOP: ses revision ...` (SE firmware revision and TOC version) and
`POWER_STOP: bkram live=<0|1> selftest=<0|1>`; the BOOT diag words 52-55 hold the restore's
BKRAM self-test result (1 ok, 2 failed), RET_CTRL and VBAT_ANA_REG1 before the re-assert, and
VBAT_ANA_REG1 after it.

Variants (v) and (vi) are the two differences from the vendor `system_off` sample not
covered by (i)-(iv); see `docs/aen-power-domains.md`.

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
  is reported as an aborted sleep (`valid=1 mode=0 wake_source=0x0`), not a wake.

## Bench contract

```
POWER_STOP: cycle<n> <check> <PASS|FAIL>
POWER_STOP: SUMMARY cycles=<n> pass=<n> fail=<n>
```

Checks, per cycle: `record_valid mode_stop wake_source restored_all bkram_counter`.
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

