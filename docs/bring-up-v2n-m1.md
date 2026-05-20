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
| DA9292 CH2                   | disabled (0.75 V DEEPX rail unused)            | enabled at bring-up via `da9292_v2n_m1_enable_deepx_rail`                          |
| TPS628640 instances on BRD_I2C | 1 optional (LPD4x_0V6 @ 0x4D)                | 4 total (adds 0x48 / 0x44 / 0x4F for DEEPX rails)                                  |
| PCIe muxes                   | not applicable                                | 2 × PI3DBS12212A; PD on Renesas P80, SEL on P95                                     |
| `M1_RESET` line              | not applicable                                | Renesas PA6 -- driven by host firmware via `chips/deepx_dxm1/`                      |
| DEEPX kernel runtime         | not applicable                                | `dx_rt_npu_linux_driver` + `libdxrt.so` from upstream `meta-deepx-m1` Yocto layer  |

## Step-by-step

### 1. Confirm DEEPX rail PMICs ACK on BRD_I2C

```c
tps628640_t t44, t48, t4f;
tps628640_init(&t44, brd_i2c, 0x44, 1050); /* DDR5_VDD       */
tps628640_init(&t48, brd_i2c, 0x48, 850);  /* VDD0V85_LPDDR  */
tps628640_init(&t4f, brd_i2c, 0x4F, 500);  /* DDR5_VDDQ_0V5  */
```

Each `_init` must return `ALP_OK` (NOT `ALP_ERR_NOT_READY`).  All
three rails self-regulate to their factory OTP voltages with no
host writes -- firmware just confirms the parts are populated.

If any one returns `ALP_ERR_NOT_READY`: probe the rail directly.
A missing population shows up as "buck instance not on the bus";
a populated buck that won't ACK is either powered down (check
EN line) or address-strapped wrong.

### 2. Bring up the DA9292 DEEPX rail (CH2)

```c
da9292_t pmic;
da9292_init(&pmic, brd_i2c, DA9292_I2C_ADDR_V2N);
da9292_v2n_m1_enable_deepx_rail(&pmic, /* timeout_us */ 50000);
```

The helper:

1. Writes `CH2_VOUT_VSEL_LO = 0.75 V` (0x96).
2. Reads back to confirm the write took.
3. Sets `CH2_EN = 1` in `PMC_CTRL_01`.
4. Polls `CH2_PG` up to `timeout_us` for the rail to be in
   regulation.

Expected: returns `ALP_OK` within ~5 ms.  Returning `ALP_ERR_TIMEOUT`
means the rail isn't coming up -- typically a downstream short on
the 0.75 V plane.

### 3. Sequence M1_RESET + PCIe muxes

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
> Board boards on different DEEPX revisions can flip the
> polarity via `deepx_dxm1_set_reset_polarity` if the silicon's
> reset polarity ever changes.

### 4. Hand off to Linux

If the SoM is running Yocto with the `meta-deepx-m1` layer wired
into `e1m-x-v2n-m1.conf`:

* `dx_rt_npu_linux_driver` opens the PCIe device at `lspci`-time.
* `dxrt_init()` from user-space succeeds; load a `.dxnn` model
  and run inferences.

If the kernel comes up but `dxrt_init()` returns an error, see the
upstream DEEPX troubleshooting docs at
[`github.com/DeepX-AI/dx_rt`](https://github.com/DeepX-AI/dx_rt).

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

* **`da9292_v2n_m1_enable_deepx_rail` succeeds but DEEPX silicon is
  flaky under load.**  Check the three TPS628640 rails -- the
  factory OTP voltages assume a specific load envelope.  Verify
  with a scope; reach for the (currently-`NOSUPPORT`)
  `tps628640_set_voltage_mv` helper once the datasheet drop
  unblocks the VSET decode TODO.

## See also

* [`vendors/deepx-dxm1/README.md`](../vendors/deepx-dxm1/README.md) --
  upstream cross-link + Yocto integration.
* `metadata/e1m_modules/v2n-m1/README.md` --
  authoritative V2N-M1 pinout delta.
