# v2n-m1-deepx-inference

DEEPX DX-M1 NPU bring-up + single inference through
`<alp/inference.h>` on a V2N-M1 SoM.

This is the in-repo half of the V2N-M1 DEEPX integration story.
The customer-side half (the DEEPX `dx_rt` runtime + Linux PCIe
driver pulled from [`github.com/DEEPX-AI`](https://github.com/DEEPX-AI))
sits outside this repo because it ships under DEEPX's
customer-only licence -- see
[`docs/vendor-partnerships.md`](../../../docs/vendor-partnerships.md)
§DEEPX for the full licence story.

## What this shows

1. **PCIe mux + DEEPX power rail bring-up -- owned entirely by U-Boot,
   before this Zephyr image ever runs.**  `board_late_init()`
   (`meta-alp-sdk/recipes-bsp/u-boot/u-boot/
   0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`) sequences the
   DA9292 CH2 = 0.75 V DEEPX rail over RIIC8/BRD_I2C, polls it to
   power-good, drives `DEEPX_CORE_0P75_EN` (`P64`) high, and only then
   releases `M1_RESET` (`PA6`) and enables the PCIe mux.  By the time
   this `m33_sm` image's `main()` runs, the rail and the PCIe link are
   already up (or the SoM was never V2N-M1 hardware and neither is
   touched).  RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
   (`metadata/e1m_modules/v2n/core-ownership.yaml`) -- the CM33 must
   never master it, so there is no Zephyr-side rail path; the
   `src/zephyr/v2n_power_mgmt.c` module (a P65-IRQ-driven attempt at
   the same sequence, never wired on any in-tree board) does not
   exist in this tree.
2. **PCIe mux + `M1_RESET` release -- already done by the time this
   image runs.**  The [`chips/deepx_dxm1/`](../../../chips/deepx_dxm1/)
   host driver wraps the PI3DBS12212 PCIe mux routing + the Renesas
   `PA6` `M1_RESET` release into one `deepx_dxm1_bring_up()` call, for
   platforms where a portable caller owns that GPIO.  On V2N-M1 it does
   NOT apply: U-Boot is the sole driver of `M1_RESET` (see point 1),
   so this example does not call it and never should.
3. **Inference handle open via the portable `<alp/inference.h>`
   surface.**  `backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1` +
   `format = ALP_INFERENCE_MODEL_DXNN`.  The real DEEPX backend
   is on the A55 Yocto image (`src/yocto/inference_deepx.cpp`,
   against `dx_rt`, which the customer pulls in per the licence
   story above); this `m33_sm` Zephyr image has no DEEPX
   backend, so the open fails here as it does under native_sim
   (see the output below).
4. **One inference invoke + result print.**

## Build

### native_sim (framing test only)

```bash
west build -b native_sim/native/64 examples/v2n/v2n-m1-deepx-inference
west build -t run
```

Expected output (every backend call returns NOSUPPORT on the
host-emulated path; the example reports the failure cleanly):

```
[deepx] v2n-m1-deepx-inference flagship
[deepx] stage 1: PCIe mux + DEEPX rail bring-up (already done by U-Boot)
[deepx] stage 2: opening DEEPX inference handle
[deepx]   open returned NULL: last_err=-8
[deepx]   (expected under native_sim and on builds without dx_rt)
[deepx] done
```

### Real V2N-M1 silicon

This `m33_sm` image does not run DEEPX inference on silicon: the rail
and PCIe link are already handled entirely by U-Boot before this image
starts (see point 1 above), this app does not call
`deepx_dxm1_bring_up()`, and it has no Zephyr DEEPX backend.  Real DEEPX
inference runs on the A55 Yocto image against `dx_rt`.  The steps
below build and flash the `m33_sm` image only.

```bash
# Pull in the DEEPX runtime per the customer-licence story
# documented in docs/vendor-partnerships.md §DEEPX.
git clone https://github.com/DEEPX-AI/dx_rt modules/dx_rt
git clone https://github.com/DEEPX-AI/dx_rt_npu_linux_driver modules/dx_rt_driver

tan build --project examples/v2n/v2n-m1-deepx-inference
west flash
```

Replace `k_placeholder_model` in `src/main.c` with your own
DXNN-compiled model (use DEEPX's `dxcom` host compiler to produce
a `.dxnn` file from an ONNX source) before flashing.

## Verification

HiL only -- the DEEPX NPU is not available under any
Zephyr-side emulator.  The V2N-M1 board file
(`alp_e1m_v2m101_m33_sm`, `zephyr/boards/alp/e1m_v2m101_m33_sm/`)
ships in-tree; once a test rig hosts a real V2N-M1 EVK, this
example flips from `build_only: true` to a positive-path Twister
scenario.

## Reference

- [`<alp/inference.h>`](../../../include/alp/inference.h) --
  portable inference surface (CPU / Ethos-U / DRP-AI /
  DEEPX-DX).
- [`<alp/chips/deepx_dxm1.h>`](../../../include/alp/chips/deepx_dxm1.h)
  -- DX-M1 host driver API (PCIe mux + reset sequencer).
- [`docs/vendor-partnerships.md`](../../../docs/vendor-partnerships.md)
  §DEEPX -- the customer-only licence story + what falls on
  the customer to integrate.
- [`docs/tutorials/16-inference-mobilenet.md`](../../../docs/tutorials/16-inference-mobilenet.md)
  -- the Ethos-U sibling tutorial; same `<alp/inference.h>`
  surface, different backend.
