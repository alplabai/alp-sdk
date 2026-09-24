# Bench bring-up — E1M-X V2N-M1 delta

Picks up where [`bring-up-v2n.md`](bring-up-v2n.md) ends.  Covers
the **delta** introduced by the V2N-M1 variant: three DEEPX-rail
PMIC instances on `BRD_I2C`, the `M1_RESET` line, the two passive
PCIe muxes, and the DEEPX kernel runtime hand-off.

> Confirm V2N base bring-up is clean before reading this doc —
> rail debugging the DEEPX path on a V2N that isn't itself
> healthy is a recipe for blaming the wrong thing.

## What's different

| Aspect                       | V2N base                                      | V2N-M1                                                                              |
|------------------------------|-----------------------------------------------|-------------------------------------------------------------------------------------|
| DEEPX silicon                | absent                                        | populated (DX-M1 BGA, on-module)                                                    |
| DA9292 CH2                   | disabled (0.75 V DEEPX rail unused)            | sequenced to 0.75 V + confirmed power-good by U-Boot's `board_late_init()` before Linux or the CM33 image ever starts |
| TPS628640 instances on BRD_I2C | 1 optional (LPD4x_0V6 @ 0x4D)                | 4 total (adds `deepx_lpddr_0v85` [address TBD, #1163] / 0x44 / 0x4F for DEEPX rails) |
| PCIe muxes                   | not applicable                                | 2 × PI3DBS12212A; PD on Renesas P80, SEL on P95                                     |
| `M1_RESET` line              | not applicable                                | Renesas PA6 -- driven by host firmware via `chips/deepx_dxm1/`                      |
| DEEPX kernel runtime         | not applicable                                | `dx_rt_npu_linux_driver` + `libdxrt.so` from upstream `meta-deepx-m1` Yocto layer  |

## Step-by-step

### 1. Confirm DEEPX rail PMICs ACK on BRD_I2C

```c
tps628640_t t44, tlpddr, t4f;
tps628640_init(&t44, brd_i2c, 0x44, 1050);       /* DDR5_VDD       */
/* deepx_lpddr_0v85's real strap is unresolved (#1163) -- do NOT hardcode
 * 0x48 here (tmp112 is confirmed 0x40, not 0x48; see docs/soms/v2n-m1.md's
 * "deepx_lpddr_0v85 strap is unresolved" section).
 * Substitute the address once the schematic confirms it. */
tps628640_init(&tlpddr, brd_i2c, DEEPX_LPDDR_0V85_ADDR_TBD, 850); /* VDD0V85_LPDDR */
tps628640_init(&t4f, brd_i2c, 0x4F, 500);        /* DDR5_VDDQ_0V5  */
```

Each `_init` must return `ALP_OK` (NOT `ALP_ERR_NOT_READY`).  All
three rails self-regulate to their factory OTP voltages with no
host writes -- firmware just confirms the parts are populated.

If any one returns `ALP_ERR_NOT_READY`: probe the rail directly.
A missing population shows up as "buck instance not on the bus";
a populated buck that won't ACK is either powered down (check
EN line) or address-strapped wrong.

### 2. DA9292 DEEPX rail (CH2) -- owned by U-Boot, nothing to do here

**U-Boot performs this step; no firmware you write needs to.**
U-Boot's `board_late_init()`
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
sequences CH2 to 0.75 V and confirms power-good over RIIC8/BRD_I2C
BEFORE Linux or the CM33 image ever starts, and only then releases
`M1_RESET` (step 3 below).  RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
(`metadata/e1m_modules/v2n/core-ownership.yaml`) -- the CM33 must never
master it, so there is no CM33 code path that could run this sequence
even if you wanted it to.

At the U-Boot prompt (or in its serial log) you should see:

```
ALP: DA9292 programmed CTRL_01=0x01 VOUT_CH2=0x96/0x96
ALP: DEEPX rail 0.75V up (PG)
```

(`CTRL_01=0x01` is CH1_EN=1 / CH2_EN=0 at the end of the program
phase -- CH2_EN is only set afterward, in the enable phase, once P65
is confirmed high.)

If instead you see `ALP: DA9292 ...` followed by `abort` or `DEEPX
rail not enabled` / `DEEPX rail disabled`, the rail failed to
sequence -- read the printed register bytes (they name exactly which
check failed: DEV_ID mismatch, STATUS_01 not clean, a CTRL_01/VOUT_CH2
readback mismatch, or CH2 not reaching power-good) and `M1_RESET`
stays asserted, so nothing past this point in the guide will work
until it's fixed.

**If the line reads `... DEEPX rail not enabled (CH2 may require
EN2/P64 high before PG -- see bring-up doc)`:** CH2_EN was written
over I2C and PG never asserted within the 20 ms poll. On this DA9292
wiring, EN2 (the CH2 hardware-enable
pin) is tied to P64 (`DEEPX_CORE_0P75_EN`) -- if EN2 gates CH2 the
same way EN1 gates CH1, the register-side `CH2_EN=1` alone is not
enough to bring CH2 up; P64 has to go high too. The sequence already
drives P64 high in step 10, but only *after* the PG poll in step 8 --
so on EN2-gated silicon, step 8 will always see PG=0 and abort before
step 10 ever runs. This is a real hardware-wiring question this patch
does not resolve on its own: if you see this message on real
silicon, confirm with a scope whether CH2 tracks P64 or the I2C
`CH2_EN` bit, and file a follow-up against #2045 rather than assuming
a rail fault.

`da9292_v2n_m1_enable_deepx_rail()` and `da9292_init()` still exist in
`chips/da9292/` and are safe to call from a bench diagnostic app for
**read-only verification** (`da9292_get_status()`,
`da9292_read_and_clear_events()`) -- but never to re-run the enable
sequence from the CM33, which would be a second, uncoordinated writer
of the same PMIC U-Boot already programmed.

### 3. Sequence M1_RESET + PCIe muxes

**Also already done by U-Boot** (same patch as step 2, right after the
rail step above succeeds) by the time this guide's steps 1-2 finish --
the C snippet below is the driver-level illustration of what
`alp_deepx_pcie_bringup()` in that patch does with raw register pokes,
useful for understanding the sequence or reusing the chip driver on a
platform where a portable caller genuinely owns `M1_RESET`, but not
something you run again on V2N-M1's CM33 (which never releases
`M1_RESET` in normal boot flow):

```c
alp_gpio_t *pd  = alp_gpio_open(/* pin_id for P80 */);
alp_gpio_t *sel = alp_gpio_open(/* pin_id for P95 */);
alp_gpio_t *rst = alp_gpio_open(/* pin_id for PA6 */);

alp_gpio_configure(pd,  ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
alp_gpio_configure(sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
alp_gpio_configure(rst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

pi3dbs12212_t mux;
pi3dbs12212_init(&mux, pd, sel);

deepx_dxm1_t dxm1;
deepx_dxm1_init(&dxm1, rst, &mux, /* deepx_path */ PI3DBS_STATE_PATH_0);
deepx_dxm1_bring_up(&dxm1, DEEPX_DXM1_DEFAULT_BOOT_US);
```

The sequencer routes the muxes to the DEEPX path, then releases
`M1_RESET`.  After `boot_us` elapses, the DEEPX silicon's internal
ROM has executed and PCIe link training can start.

> **`M1_RESET` polarity is ACTIVE-LOW on V2N-M1** -- this is the
> driver's default, so no `_set_reset_polarity` call is required.
> Boards on different DEEPX revisions can flip the
> polarity via `deepx_dxm1_set_reset_polarity` if the silicon's
> reset polarity ever changes.

### 4. Hand off to Linux

If the SoM is running Yocto with the `meta-deepx-m1` layer wired
into `e1m-v2m101-a55.conf`:

* `dx_rt_npu_linux_driver` opens the PCIe device at `lspci`-time.
* `dxrt_init()` from user-space succeeds; load a `.dxnn` model
  and run inferences.

If the kernel comes up but `dxrt_init()` returns an error, see the
upstream DEEPX troubleshooting docs at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).

## Bring-up regression checks

After every change in the bring-up flow, re-run these in order:

1. Power-on idle current under 500 mA (DEEPX adds ~150 mA static).
2. ACT88760 + DA9292 status: no `thermal_warning`, no event latches.
3. DA9292 CH2 in regulation: `da9292_get_status().ch2_pg == true`.
4. Three DEEPX TPS628640 instances ACK at their addresses.
5. `lspci` lists the DEEPX device.
6. `dxrt_init()` returns success.
7. A reference `dx_app` inference runs to completion.

## Common gotchas

* **DEEPX rails come up but PCIe link never trains.**  Almost
  certainly an `M1_RESET` polarity mismatch.  Toggle the polarity
  flag in [`<alp/chips/deepx_dxm1.h>`](../include/alp/chips/deepx_dxm1.h)
  and retest.

* **PCIe link trains but the kernel driver reports BAR errors.**
  The muxes may be on the wrong path (E1M edge instead of DEEPX);
  check `PI3DBS_STATE_PATH_0` matches the board's silk-screen.

* **U-Boot logs `ALP: DEEPX rail 0.75V up (PG)` but DEEPX silicon is
  flaky under load.**  Check the three TPS628640 rails -- the
  factory OTP voltages assume a specific load envelope.  Verify
  with a scope, then retune with `tps628640_set_voltage_mv()`.
  The VSET decode it needs is implemented (TI SLVSEI1C): the
  helper range-checks against `TPS628640_VOUT_BASE_MV` (400 mV)
  and `TPS628640_VOUT_MAX_MV` (1675 mV), encodes
  `(mv - 400) / 5` and writes `TPS628640_REG_VOUT1`.  Nothing
  in-tree calls it at boot, so the four instances still come up
  on their factory OTP voltages -- program them from the
  bring-up path if a rail needs to differ.

## See also

* [`vendors/deepx-dxm1/README.md`](../vendors/deepx-dxm1/README.md) --
  upstream cross-link + Yocto integration.
* `metadata/e1m_modules/v2n-m1/README.md` --
  authoritative V2N-M1 pinout delta.
