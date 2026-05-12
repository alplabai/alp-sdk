# E1M-X V2N-M1 family

> V2N + on-module **DEEPX DX-M1** NPU.  AI-accelerator variant.

## SKUs

| SKU            | Memory                                | Status     |
|----------------|---------------------------------------|------------|
| `E1M-V2M101`   | 32 Gbit LPDDR4X + 32 Gbit eMMC + DX-M1| production |
| `E1M-V2M102`   | 64 Gbit LPDDR4X + 64 Gbit eMMC + DX-M1| production |

## What's different from V2N base

V2N-M1 inherits the full V2N base module (see [`v2n.md`](v2n.md))
and adds:

| Component                | Where + how                                                |
|--------------------------|------------------------------------------------------------|
| **DEEPX DX-M1 NPU**      | On-module, PCIe                                            |
| `M1_RESET`               | Renesas-side GPIO controlling DX-M1 reset (active-low)     |
| 2 × PI3DBS12212A muxes   | Switch PCIe routing between DEEPX and the E1M edge         |
| 0.75 V DEEPX rail        | DA9292 CH2 (disabled on V2N base; brought up by FW on M1)  |
| 3 × TPS628640 bucks      | DDR5/LPDDR rails for DEEPX (`0x44 / 0x48 / 0x4F`)          |

## DEEPX bring-up

Four-step sequence host firmware must run **after** the Renesas
side boots and **before** the Linux kernel attempts to open the
PCIe device:

1. **Enable the 0.75 V DEEPX rail** via the secondary PMIC's CH2.
2. **ACK-probe** the three DEEPX TPS628640 instances at
   `0x44 / 0x48 / 0x4F` to confirm population (self-regulating).
3. **Route the PCIe muxes** to the DEEPX path with the PI3DBS12212A
   driver (PD pin on Renesas `P80`, SEL pin on `P95`).
4. **Release `M1_RESET`** (Renesas `PA6`; active-low).

The `chips/deepx_dxm1/` driver wraps steps 3-4 into a single
[`deepx_dxm1_bring_up(&ctx, DEEPX_DXM1_DEFAULT_BOOT_US)`](../../include/alp/chips/deepx_dxm1.h)
call.  Steps 1-2 stay caller-orchestrated because the secondary
PMIC + DEEPX bucks have their own driver APIs.

Walk-through with code: [`docs/bring-up-v2n-m1.md`](../bring-up-v2n-m1.md).

## DEEPX runtime

The DEEPX silicon's userland API (`libdxrt.so`) is upstream at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).
The Yocto layer that brings it into your image is wired in
`yocto/meta-alp/conf/machine/e1m-x-v2n-m1.conf` and references
`github.com/DEEPX-AI/meta-deepx-m1`.

Integration cross-link: [`vendors/deepx-dxm1/README.md`](../../vendors/deepx-dxm1/README.md).

## Example apps targeting V2N-M1

All V2N examples apply.  Plus DEEPX-specific examples (more to
come):

| Example                          | What you'll see                                             |
|----------------------------------|-------------------------------------------------------------|
| `v2n-pmic-rail-monitor`          | Shows the three DEEPX TPS628640 instances ACKing on BRD_I2C.|

## Common gotchas

| Symptom                                              | Cause + fix                                                            |
|------------------------------------------------------|------------------------------------------------------------------------|
| `da9292_v2n_m1_enable_deepx_rail` -> `ALP_ERR_TIMEOUT` | 0.75 V plane shorted; probe the rail directly.                       |
| DEEPX rails up but PCIe link never trains            | `M1_RESET` polarity wrong -- the driver default is active-low; carrier may need override via `deepx_dxm1_set_reset_polarity`. |
| PCIe link trains but kernel driver reports BAR errors| PCIe muxes on the wrong path -- check `PI3DBS_STATE_PATH_0` matches your carrier's silk-screen. |
| `dxrt_init()` returns an error                       | Check the DEEPX kernel driver (`dx_rt_npu_linux_driver`) is loaded.    |

## See also

* [`v2n.md`](v2n.md) -- the base SoM.
* [`../bring-up-v2n-m1.md`](../bring-up-v2n-m1.md) -- bench bring-up.
* [`../pmic-rails.md`](../pmic-rails.md) -- DEEPX-specific rails covered in §"Per-rail table".
