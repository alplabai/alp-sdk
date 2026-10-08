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

Flash it to MRAM (Flow D), **detach the J-Link** (the backend refuses to sleep with a
debugger attached and returns `ALP_ERR_BUSY`), and read the console on the E1M edge
UART0 at 115200 8N1. Start from a **cold power cycle**: the counter lives in SRAM that
a reset keeps and a power cycle loses.

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
