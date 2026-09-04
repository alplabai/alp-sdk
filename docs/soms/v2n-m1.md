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
| 3 × TPS628640 bucks      | DDR5/LPDDR rails for DEEPX (`0x44` / `0x4F` fixed; third strap **unresolved**, see below) |

## DEEPX bring-up

Four-step sequence host firmware must run **after** the Renesas
side boots and **before** the Linux kernel attempts to open the
PCIe device:

1. **Enable the 0.75 V DEEPX rail** via the secondary PMIC's CH2.
2. **ACK-probe** the DEEPX TPS628640 instances at `0x44` / `0x4F`
   to confirm population (self-regulating).  The third DEEPX buck
   (`deepx_lpddr_0v85`) has no confirmed address to probe -- see
   the strap note below before writing bring-up code against it.
3. **Route the PCIe muxes** to the DEEPX path with the PI3DBS12212A
   driver (PD pin on Renesas `P80`, SEL pin on `P95`).
4. **Release `M1_RESET`** (Renesas `PA6`; active-low).

### `deepx_lpddr_0v85` strap is unresolved (#1163)

The third DEEPX buck (`tps628640`, role `deepx_lpddr_0v85`) has no
settled I2C address on the V2M pair --
`metadata/e1m_modules/E1M-V2M101.yaml` / `E1M-V2M102.yaml` record it
as `address_7bit: "TBD"`, not `0x48`.  The chip's own default strap
*is* `0x48`, but that collides with the on-module `tmp112`
temperature sensor, which is confirmed at `0x48` on the same bus on
all four V2N-family SKUs (V2N101/102 declare the same `tmp112` at
`0x48`, and all four SKUs share one PCB, so it's a single physical
net) -- see [#1163](https://github.com/alplabai/alp-sdk/issues/1163)
and [#1845](https://github.com/alplabai/alp-sdk/issues/1845). Which
part is re-strapped on the real V2M schematic, and to what, is not
known yet. **Do not probe `0x48` expecting the DEEPX buck** -- on
this bus `0x48` is `tmp112`; treat `deepx_lpddr_0v85`'s address as
unknown until the schematic confirms it, and do not hardcode `0x48`
for it in bring-up code.

The `chips/deepx_dxm1/` driver wraps steps 3-4 into a single
[`deepx_dxm1_bring_up(&ctx, DEEPX_DXM1_DEFAULT_BOOT_US)`](../../include/alp/chips/deepx_dxm1.h)
call.  Steps 1-2 stay caller-orchestrated because the secondary
PMIC + DEEPX bucks have their own driver APIs.

Walk-through with code: [`docs/bring-up-v2n-m1.md`](../bring-up-v2n-m1.md).

## DEEPX runtime

The DEEPX silicon's userland API (`libdxrt.so`) is upstream at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).
The Yocto layer that brings it into your image is wired in
`meta-alp-sdk/conf/machine/e1m-v2m101-a55.conf` and references
`github.com/DEEPX-AI/meta-deepx-m1`.

Integration cross-link: [`vendors/deepx-dxm1/README.md`](../../vendors/deepx-dxm1/README.md).

## Example apps targeting V2N-M1

All V2N examples apply.  DEEPX-specific examples land separately
as the NPU integration matures.

## Common gotchas

| Symptom                                              | Cause + fix                                                            |
|------------------------------------------------------|------------------------------------------------------------------------|
| `da9292_v2n_m1_enable_deepx_rail` -> `ALP_ERR_TIMEOUT` | 0.75 V plane shorted; probe the rail directly.                       |
| DEEPX rails up but PCIe link never trains            | `M1_RESET` polarity wrong -- the driver default is active-low; board may need override via `deepx_dxm1_set_reset_polarity`. |
| PCIe link trains but kernel driver reports BAR errors| PCIe muxes on the wrong path -- check `PI3DBS_STATE_PATH_0` matches your board's silk-screen. |
| `dxrt_init()` returns an error                       | Check the DEEPX kernel driver (`dx_rt_npu_linux_driver`) is loaded.    |

## See also

* [`v2n.md`](v2n.md) -- the base SoM.
* [`../bring-up-v2n-m1.md`](../bring-up-v2n-m1.md) -- bench bring-up.
