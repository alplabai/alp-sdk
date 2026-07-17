# aen-rtc-regcheck -- LPRTC (snps,dw-apb-rtc) counter readout

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app binds the always-on **LPRTC** at `lprtc@42000000` through the
portable Zephyr `counter_*` class API (over the alp-sdk Tier-2
`counter_dw_rtc` driver) and proves the count **advances** on silicon --
both in firmware (`counter_get_value()` x2 with a busy-wait between) and
over J-Link (`mem32 0x42000000`, the `CCVR` register).

## IMPORTANT: this is a COUNTER, not a calendar RTC

The Alif LPRTC is a **Synopsys DesignWare APB RTC** -- which, despite the
name, is a Zephyr **counter-class** device: a free-running 32-bit
up-counter (`CCVR`) with a single compare/alarm channel (`CMR`). It has
**no battery-backed date/time registers**. So it does **NOT** satisfy the
calendar `alp_rtc_*` surface (`include/alp/rtc.h`,
`alp_rtc_time_t` year/month/day/hour/min/sec) as-is.

The alp-sdk on-silicon RTC backend
(`src/backends/rtc/zephyr_drv.c`) binds the Zephyr **RTC class**
(`rtc_set_time` / `rtc_get_time`, `struct rtc_time`) via the
`alp-rtc<N>` alias -- which will **not** bind a counter device. This
example therefore exercises the **COUNTER** path only, surfacing the
LPRTC via the `alp-counter` alias.

### The counter -> calendar shim (now authored: `aen-rtc-calendar`)

Making `include/alp/rtc.h` work over the LPRTC needs a **counter ->
calendar shim backend** -- now implemented in
`src/backends/rtc/lprtc_calendar_shim.c` (selected on `silicon_ref`
`"alif:ensemble:e8"`) and exercised by the
[`aen-rtc-calendar`](../aen-rtc-calendar/) example. It works exactly as
sketched here:

- Store an **epoch base** (UNIX seconds at the moment the counter was
  last set) in software -- the DW-APB-RTC has no hardware to hold it
  across resets.
- `alp_rtc_set_time()` = compute the epoch from the supplied
  `alp_rtc_time_t`, snapshot `counter_get_value()`, store
  `(epoch_base, tick_snapshot)`.
- `alp_rtc_get_time()` = `epoch_base + (counter_get_value() -
  tick_snapshot) / 32768`, then `gmtime`-style decompose to
  `alp_rtc_time_t`.

The **only** piece still TBD is **retained-storage persistence**: the
shim keeps `(epoch_base, tick_snapshot)` in RAM, so the wall-clock
resets to the epoch base on every cold boot until `set_time` runs again.
The retained target (battery-backed VBAT scratch vs MRAM vs filesystem)
is a SoM-policy decision and is **TBD**.

This regcheck still validates the raw **counter** underneath that shim:
the LPRTC reached as an `alp_counter` / Zephyr counter device.

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| LPRTC node | `rtc0: lprtc@42000000` `snps,dw-apb-rtc` | fork `dts/arm/alif/ensemble/common/e1.dtsi` (commit `da4a9034`), transcribed verbatim into `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` |
| reg | `0x42000000` size `0x1000` | same |
| IRQ | `58` (fork `ALIF_DEFAULT_IRQ_PRIORITY` -> literal `3` here) | same |
| clock-frequency | `32768` Hz | same |
| prescaler / load-value | `0` / `0` | same |
| `CCVR` (counter value) offset | `+0x00` -> J-Link `mem32 0x42000000` | vendored header `counter_dw_rtc.h` `DW_RTC_REG_CCVR (0x00)` |
| Driver IP | counter-class, `DT_DRV_COMPAT snps_dw_apb_rtc` | vendored `counter_dw_rtc.c` (fork commit `da4a9034`) |
| Distinct from upstream | upstream `counter_dw_timer.c` is `snps_dw_timers` (DW APB **Timers**, a different IP) | `<zephyr>/drivers/counter/counter_dw_timer.c` |
| Kconfig | `COUNTER_RTC_SNPS_DW`, `depends on DT_HAS_SNPS_DW_APB_RTC_ENABLED` | fork `drivers/counter/Kconfig.dw_rtc`; mirrored in `zephyr/kconfigs/vendor-alif-peripherals.kconfig` |

The driver `.c`/`.h` register model is vendored **verbatim** from the
fork (only the provenance header is alp-sdk-added) -- **no register
value invented**.

## BLOCKER: the VBAT LPRTC clock-gate (bench-TBD)

The LPRTC clock is gated from the always-on **VBAT** domain. In the
Alif fork, the SoC layer enables it when `rtc0` is `okay`:

```
/* soc/alif/ensemble/common/soc_common.c (fork, commit da4a9034) */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(rtc0), okay)
        sys_write32(0x1, VBAT_LPRTC0_CLK_EN);   /* VBAT_BASE+0x10 = 0x1A609010 */
#endif
```

**alp-sdk builds against upstream Zephyr v4.4**, whose ensemble SoC
layer (`soc/alif/ensemble/common/soc.c`, 12 lines, only
`soc_reset_hook`) does **not** perform this write -- and the
`counter_dw_rtc` driver does **not** write VBAT either (it relies on the
SoC code). So on the alp-sdk build path the gate is enabled by **no
visible code**.

Two possibilities, to settle on the bench:

1. The SES / always-on VBAT domain already leaves the LPRTC clock
   running -> `CCVR` advances, `RESULT PASS`, nothing more to do.
2. The gate is off -> `CCVR` never advances, `RESULT FAIL`. The fix is
   to perform the `VBAT_LPRTC0_CLK_EN` write from the alp-sdk SoC
   bring-up (the macro is already defined in
   `zephyr/soc-bridge/alif/soc_common.h:46`,
   `VBAT_BASE 0x1A609000`). **Do NOT add this write speculatively
   without the Alif TRM confirming the bit** -- the bit position
   (`VBAT_RTC_CLK_EN_CLK_EN_BIT = 0`) is carried in the vendored header
   for that future use.

The `RESULT FAIL` branch in `src/main.c` calls this out explicitly.

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-rtc-regcheck -d build/aen-rtc-regcheck -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
    -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-rtc-regcheck/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-rtc-regcheck/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run. The overlay retargets `zephyr,flash = &itcm` for the RAM-run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x42000000` (LPRTC `CCVR`) across the two
   readback windows must advance (~32768 ticks/s -> ~3277 ticks over the
   100 ms in-firmware gap).
5. `RESULT PASS:` = counter advances. `RESULT FAIL: ... did not
   advance` -> check the VBAT clock-gate (above).

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017 Tier-2 (consume an opt-in fork driver), INTERIM.
