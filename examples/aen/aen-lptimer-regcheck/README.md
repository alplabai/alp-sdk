# aen-lptimer-regcheck -- LPTIMER (alif,lptimer) counter readout

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app binds the always-on **LPTIMER** at `lptimer@42001000` (channel 0)
through the portable Zephyr `counter_*` class API (over the alp-sdk
Tier-1.5 `counter_alif_lptimer` driver) and proves the count **advances**
on silicon -- both in firmware (`counter_get_value()` x2 with a busy-wait
between) and over J-Link (`mem32 0x42001004`, the channel-0 `CURRENTVAL`
register).

## IMPORTANT: this is a DOWN-counter, and a DISTINCT block

The Alif LPTIMER is a low-power timer block holding four independent
32-bit **DOWN-counters** (channels 0..3). In free-running mode each
channel loads `0xFFFFFFFF` and counts **down**, reloading `0xFFFFFFFF` on
underflow. So `counter_get_value()` returns a value that **decreases**
over time -- "advances" here means the count **changes** (first read >
second read), so the advance delta is `(v1 - v2)`.

It is a **separate IP** from:

- the **LPRTC** (`lprtc@42000000`, `snps,dw-apb-rtc`, an UP-counter --
  see `aen-rtc-regcheck`), and
- the **UTIMER** counter (`alif,utimer-counter`).

Each LPTIMER channel has its own NVIC line (60..63 on E8). This node
binds **channel 0** (IRQ 60); the count is surfaced to app code as an
`alp_counter` via the `alp-counter` alias.

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| LPTIMER node | `lptimer0: lptimer@42001000` `alif,lptimer` | Alif DFP `Device/soc/AE822FA0E5597/include/rtss_he/soc.h` `LPTIMER_BASE 0x42001000`; node in `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` |
| reg | `0x42001000` size `0xC0` | Alif DFP soc.h `LPTIMER_Type` Size = 192 (0xC0) |
| IRQ (ch0..3) | `60, 61, 62, 63` (node uses ch0 = 60) | Alif DFP soc.h `LPTIMER0..3_IRQ_IRQn = 60..63` |
| clock-source / clock-frequency | `0` (32 kHz) / `32768` Hz | Alif DFP `sys_ctrl_lptimer.h` `LPTIMER_CLK_SOURCE_32K = 0` |
| `CURRENTVAL` (ch0 count) offset | base + `0`*0x14 + `0x04` -> J-Link `mem32 0x42001004` | clean-room header `counter_alif_lptimer.h` `LPTIMER_CH_CURRENTVAL (0x04)` (Alif DFP soc.h `LPTIMER_CURRENTVAL` @0x04) |
| `CONTROLREG` bits | `ENABLE 0x01`, `MODE 0x02`, `INT_MASK 0x04` | Alif DFP `drivers/include/lptimer.h` `LPTIMER_CONTROL_REG_*` |
| free-run reload | `0xFFFFFFFF` | Alif DFP `lptimer.h` `lptimer_load_max_count` |
| VBAT clock select | `TIMER_CLKSEL` @ `0x1A609004`, 2-bit field at `(ch << 2)` | Alif DFP `sys_ctrl_lptimer.h` `select_lptimer_clk`; `VBAT_BASE 0x1A609000` + `0x04` |
| Driver IP | counter-class, `DT_DRV_COMPAT alif_lptimer` | clean-room `counter_alif_lptimer.c` |
| Kconfig | `COUNTER_ALIF_LPTIMER`, `depends on DT_HAS_ALIF_LPTIMER_ENABLED` | `zephyr/Kconfig` |

The driver register model is **clean-room** -- every value transcribed
(value-only, not copied) from the proprietary Alif DFP with an inline
citation; **no register value invented**. The `alifsemi/zephyr_alif`
fork ships **no** Zephyr LPTIMER driver and **no** `alif,lptimer`
binding, and hal_alif exposes no Zephyr device for it -- hence Tier-1.5
(a thin in-tree driver), not Tier-2.

## BLOCKER: the VBAT LPTIMER clock source (bench-TBD)

The LPTIMER input clock is selected per channel via the always-on
**VBAT** `TIMER_CLKSEL` register (`0x1A609004`); the driver performs that
select in `init` from the node's `clock-source` (0 = 32 kHz). Whether the
selected VBAT LF source is itself **running** on the alp-sdk
upstream-Zephyr build path is **bench-TBD**:

1. The always-on VBAT domain already leaves the 32 kHz source running ->
   `CURRENTVAL` advances, `RESULT PASS`.
2. The source is off -> `CURRENTVAL` never advances, `RESULT FAIL`. The
   `RESULT FAIL` branch in `src/main.c` calls this out explicitly. **Do
   NOT add further VBAT writes speculatively without the Alif TRM
   confirming the bits.**

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-lptimer-regcheck -d build/aen-lptimer-regcheck -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
    -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-lptimer-regcheck/boards/alp_e1m_aen801_m55_he.overlay
```

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-lptimer-regcheck/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run. The overlay retargets `zephyr,flash = &itcm` for the RAM-run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x42001004` (LPTIMER ch0 `CURRENTVAL`) across the
   two readback windows must **decrease** (~32768 ticks/s -> ~3277 ticks
   over the 100 ms in-firmware gap).
5. `RESULT PASS:` = counter advances. `RESULT FAIL: ... did not advance`
   -> check the VBAT clock source (above).

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017 Tier-1.5 (thin in-tree Zephyr driver over the LPTIMER
registers), INTERIM, task #21.
