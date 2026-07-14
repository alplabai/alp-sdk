# aen-rtc-calendar -- alp_rtc_* calendar over the LPRTC counter (shim)

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app proves the **portable calendar surface** `alp_rtc_set_time()` /
`alp_rtc_get_time()` (`include/alp/rtc.h`) works on the E8 -- even though
the E8 has **no Zephyr-RTC-class peripheral**. The only timekeeping
hardware is the always-on **LPRTC** (`lprtc@42000000`,
`snps,dw-apb-rtc`), which is a bare free-running 32-bit **counter**
(`CCVR` @ 32768 Hz), not a date/time register file.

## The shim: counter -> calendar, in pure SDK C

The `aen-rtc-regcheck` example (next door) proved the raw LPRTC
**counter** advances. This example proves the **calendar shim** layered
on top of it: `src/backends/rtc/lprtc_calendar_shim.c`, an alp-sdk `rtc`
backend selected on `silicon_ref` `"alif:ensemble:e8"`.

It is **pure SDK C over the already-binding counter** -- it introduces
**no new Zephyr driver** (ADR 0017: stay above the vendor SDK; consume
the Tier-2 `counter_dw_rtc` driver as-is). The model:

- Keep an **epoch base** (UNIX seconds at the last `set_time`) and a
  **counter snapshot** (`counter_get_value()` at that instant) in RAM.
- `alp_rtc_set_time()` = convert the calendar fields to UNIX seconds,
  snapshot the counter, store `(epoch_base, tick_snapshot)`.
- `alp_rtc_get_time()` = `epoch_base + (counter_now - tick_snapshot) /
  freq`, then decompose to calendar fields. `freq =
  counter_get_frequency()` (32768 Hz on this node). Unsigned 32-bit
  subtraction is correct across a single CCVR wrap (~36 h).

The calendar <-> UNIX-seconds math is a clean-room implementation of
Howard Hinnant's public-domain civil-date algorithm -- **no hardware
value invented**.

### What this app does

1. `alp_rtc_open(0)` -- selection lands on the E8 shim backend.
2. `alp_rtc_set_time(2026-06-18 14:30:00)`.
3. `alp_rtc_get_time()`, busy/sleep ~2 s, `alp_rtc_get_time()` again.
4. Confirm the calendar **advanced** by ~2 s -> one `RESULT PASS:` /
   `RESULT FAIL:` line in `ram_console_buf`.

Ground truth (independent of any printk): read the LPRTC `CCVR` over
J-Link (`mem32 0x42000000`) across the same window -- it must advance
~32768 ticks/s.

## TBD: retained-storage persistence (not in this change)

The shim's `epoch_base` + `tick_snapshot` live in **RAM only**. The
DW-APB-RTC has **no battery-backed scratch** to hold them, so the
wall-clock resets to the compiled epoch (1970-01-01) on every **cold
boot** until `set_time` runs again. Persisting `(epoch_base,
tick_snapshot)` to retained storage (battery-backed VBAT scratch vs MRAM
vs a filesystem) is a **SoM-policy decision and is TBD** -- it does not
affect this in-session advance proof. The shim is the in-RAM half a
persistence layer would wrap.

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| LPRTC node | `rtc0: lprtc@42000000` `snps,dw-apb-rtc` | `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` (transcribed from fork `e1.dtsi`) |
| reg | `0x42000000` size `0x1000` | same |
| clock-frequency | `32768` Hz | same |
| `CCVR` (counter value) offset | `+0x00` -> J-Link `mem32 0x42000000` | vendored header `counter_dw_rtc.h` `DW_RTC_REG_CCVR (0x00)` |
| counter freq read | `counter_get_frequency()` (32768 Hz) | `<zephyr>/include/zephyr/drivers/counter.h` (pinned v4.4.0) |
| backend `silicon_ref` | `"alif:ensemble:e8"` | `include/alp/soc_caps.h` (`ALP_SOC_REF_STR`, E8 block) |
| `ALP_SOC_RTC_COUNT` | `1` | same |
| shim time source alias | `alp-lprtc-counter = &rtc0` | this overlay |

## BLOCKER (separate TBD): the VBAT LPRTC clock-gate

The LPRTC clock is gated from the always-on **VBAT** domain
(`VBAT_LPRTC0_CLK_EN`, `0x1A609010`). On the bench it is **already on**
(the counter regcheck saw `counter_start -> -EALREADY` and `CCVR`
advancing), so the shim assumes a running counter and does **not** write
VBAT. If the calendar never advances here, that gate is the likely cause
-- enable it from SoC bring-up with the Alif TRM confirming the bit. **Do
NOT poke VBAT from this shim or app.** This is a documented, separate
bench check (see the backend header + `aen-rtc-regcheck/README.md`).

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow); alp-sdk is
added as an EXTRA module so the `alp_rtc_*` dispatcher + the E8 calendar
shim backend link in:

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-rtc-calendar -d build/aen-rtc-calendar -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

The overlay is named for the fully-qualified board
(`boards/alp_e1m_aen801_m55_he.overlay`), so it auto-applies.

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-rtc-calendar/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run. The overlay retargets `zephyr,flash = &itcm` for the RAM-run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x42000000` (LPRTC `CCVR`) across the two
   readback windows must advance.
5. `RESULT PASS:` = the calendar advances with the counter.
   `RESULT FAIL: ... did not advance` -> check the VBAT clock-gate.

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017: the shim is **pure SDK C** above the Tier-2 `counter_dw_rtc`
driver; no driver rewritten.
