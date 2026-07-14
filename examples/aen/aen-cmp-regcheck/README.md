# aen-cmp-regcheck -- HSCMP (alif,cmp) internal-reference smoke

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app binds the high-speed analog comparator **cmp0** at
`cmp@49023000` through the portable Zephyr **comparator** class API
(`comparator_get_output` / `comparator_set_trigger` /
`comparator_set_trigger_callback`) over the alp-sdk Tier-2
`comparator_alif` driver, and proves the comparator path is **alive** on
silicon -- both in firmware (a full call sequence, each return code
checked) and over J-Link (`mem32 0x49023018`, the `CMP_STATUS` register).

## INTERNAL-reference smoke -- no external wiring

The comparator's **negative input** is set, in the SoC dtsi node, to the
on-chip **DAC6 programmable reference** (`negative_input = "CMP_NEG_IN3"`
-> `COMP_HS_IN_M_SEL = 0x3 = DAC6`). The driver enables DAC6 through the
hal_alif `analog_ctrl` helper -- on AE822 (aliasing-mode silicon) via
`enable_dac6_ref_voltage_alias_mode()` against the separate DAC6 / ADC_VREF
blocks -- so the minus terminal carries a known **internal** reference
(~0.9 V) with **no bench wiring**. The smoke then drives the comparator
through the portable API and confirms every call returns a valid,
non-error result.

The comparator's NVIC line is connected (`IRQ_CONNECT`) at device init but
**not enabled** there: the HSCMP analog core is live the moment it is
enabled, so an already-asserted event on a floating/unbiased input would
self-retrigger an ISR storm if the line were enabled before a trigger is
armed -- which previously hung POST_KERNEL init (no boot banner). The
driver now enables the NVIC line only inside `comparator_set_trigger()`
(for a non-`NONE` trigger) and disables it on `NONE`, mirroring the
upstream `comparator_stm32_comp.c` convention.

The **output level** is **not** asserted: the positive input is the
`CMP0_IN0` analog pad, which is **unrouted** on this bench, so its voltage
vs the DAC6 reference is indeterminate -- whatever bit comes back is
**reported, not failed**. The PASS gate is *"the driver drove the HSCMP IP
through the portable `comparator_*` API and every call returned a valid,
non-error result"* -- exactly the bar `aen-adc-regcheck` uses for an
unrouted pad.

### TBD: the external input pin + threshold (bench config)

Comparing against an **external** voltage (and asserting the crossing)
needs bench wiring that is deliberately **not** set here:

- the `CMP0_IN0..IN3` input pad muxed via an `&pinctrl_cmp0` group --
  **not defined in this tree** (the fork references it but the pad map is
  a bench unknown on this batch);
- an external source on that pad at a known threshold vs the comparator
  reference.

Both are **TBD** -- do **not** invent a pinctrl group or a pad mapping.
Until they are wired, the comparator is validated **only** against the
internal DAC6 reference, which is exactly what this regcheck does.

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| CMP node | `cmp0: cmp@49023000` `alif,cmp` | fork `dts/arm/alif/ensemble/common/e1.dtsi` cmp0, transcribed into `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` |
| reg (`cmp_reg`/`config_reg`) | `0x49023000` size `0x1000` / `0x100` | same; base from alif-dfp `Device/soc/AE822FA0E5597/include/rtss_he/soc.h` `CMP0_BASE 0x49023000` |
| IRQ | `167` (cmp0; cmp1 168, cmp2 169, cmp3 170) | fork e1.dtsi cmp0..cmp3 |
| `CMP_COMP_REG1` (config+enable) offset | `+0x00` -> `mem32 0x49023000` | alif-dfp `Device/soc/AE822FA0E5597/include/rtss_he/soc.h` `CMP_Type` |
| `CMP_STATUS` (output) offset | `+0x18` -> `mem32 0x49023018` | same; `CMP_STATUS` @ `0x18` |
| `COMP_HS_EN` (enable) bit | bit `28` | alif-dfp `drivers/include/sys_ctrl_cmp.h` `CMP0_ENABLE (1U<<28)` + Debug/SVD |
| `COMP_HS_IN_M_SEL` (neg) field / DAC6 value | bits `[3:2]` / `0x3` = DAC6 | alif-dfp Debug/SVD `CMP_COMP_REG1.COMP_HS_IN_M_SEL` |
| `COMP_HS_IN_P_SEL` (pos) field | bits `[1:0]` | same SVD register |
| `COMP_HS_HYST` field | bits `[6:4]`, 6 mV/step | same SVD register |
| `CMP_STATUS.CMP_VALUE` (output bit) | bit `0` | alif-dfp Debug/SVD `CMP_STATUS` |
| `CMP_INTERRUPT_MASK`/`_STATUS` offsets | `+0x24` / `+0x20` | alif-dfp `CMP_Type`; mask=`0`=enabled, status write `0x01`=clear (`drivers/include/cmp.h`) |
| CMP clock-gate | `CLKCTRL_PER_SLV->CMP_CTRL` (`0x4902F038`) bit 0 | `zephyr/soc-bridge/alif/soc_common.h:55` + alif-dfp `sys_ctrl_cmp.h` `CMP_CTRL_CMP0_CLKEN (1U<<0)` |
| DAC6 internal ref (AE822 aliasing) / analog LDO | `enable_dac6_ref_voltage_alias_mode()` / `enable_analog_peripherals()` | hal_alif `drivers/analog/include/analog_ctrl.h`; aliasing branch `alif-dfp drivers/include/sys_ctrl_analog.h` |
| NVIC line gating | `IRQ_CONNECT` at init, `irq_enable`/`irq_disable` in `set_trigger` | `zephyr/drivers/comparator/comparator_alif.c`; convention from upstream `comparator_stm32_comp.c` |
| Class API | `comparator_get_output/set_trigger/...` | upstream Zephyr v4.4 `<zephyr/drivers/comparator.h>` |
| Driver tier | clean-room driver (hal_alif + the available fork ship no comparator `.c`) | `zephyr/drivers/comparator/comparator_alif.c` |

The register sequencing is **authored clean-room** from the cited DFP
definitions -- **no register value invented**. The v4.4 call sequence
mirrors the sdk-alif reference `samples/drivers/cmp/src/main.c`.

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-cmp-regcheck -d build/aen-cmp-regcheck -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
    -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-cmp-regcheck/boards/alp_e1m_aen801_m55_he.overlay
```

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-cmp-regcheck/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run. The overlay retargets `zephyr,flash = &itcm` for the RAM-run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x49023000` (`CMP_COMP_REG1`) bit 28
   (`COMP_HS_EN`) must read 1 after init; `mem32 0x49023018`
   (`CMP_STATUS`) bit 0 (`CMP_VALUE`) is the live comparator output.
5. `RESULT PASS:` = the driver drove the HSCMP IP through the portable
   `comparator_*` API with every call returning a valid, non-error
   result. `RESULT FAIL: ...` -> the failing call + its `-errno` is
   printed.

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017 Tier-2 (clean-room driver over the hal_alif analog lib),
INTERIM.
