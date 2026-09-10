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

1. **PCIe mux + DEEPX power rail bring-up.**  The
   [`v2n_power_mgmt.c`](../../../src/zephyr/v2n_power_mgmt.c)
   module (landed in §C.28) is written to respond to the board's
   `DEEPX_PWR_EN_REQ` rising edge on `P65`, bring up the
   DA9292 CH2 = 0.75 V DEEPX rail, then drive
   `DEEPX_CORE_0P75_EN` (`P64`) high from its own SYS_INIT hook.
   **It is not active on `alp_e1m_v2m101_m33_sm` today:** the
   board devicetree defines neither the `v2n-deepx-pwr-en-req`
   nor the `v2n-deepx-core-0p75-en` alias, so the module compiles
   to its `ALP_ERR_NOSUPPORT` stub and nothing brings the rail up
   before `main()` runs (#2045).  Separately, this board's
   `CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID` stays `-1`, the
   in-tree default, which would fail init at runtime with
   `ALP_ERR_NOSUPPORT` before P65 is armed even if the aliases
   were added (#2044).  **Do not wire or flash this onto an
   E1M-V2M101 without the bench-safety section of #2045:** the
   DA9292-AROVx OTP variant boots `PMC_CTRL_01 = 0x80`
   (`CH2_VSTEP=1`), so writing the VSTEP=0-range 0.75 V byte
   (`0x96`) while `CH2_VSTEP` is still 1 yields 1.50 V on the
   DEEPX rail.
2. **PCIe mux + `M1_RESET` release.**  The
   [`chips/deepx_dxm1/`](../../../chips/deepx_dxm1/) host
   driver wraps the PI3DBS12212 PCIe mux routing + the
   Renesas `PA6` `M1_RESET` release into one
   `deepx_dxm1_bring_up()` call.  This example does not call it.
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
[deepx] stage 1: PCIe mux + power_mgmt bring-up (supervisor-side)
[deepx] stage 2: opening DEEPX inference handle
[deepx]   open returned NULL: last_err=-8
[deepx]   (expected under native_sim and on builds without dx_rt)
[deepx] done
```

### Real V2N-M1 silicon

This `m33_sm` image does not run DEEPX inference on silicon: it does
not bring the DEEPX rail up (#2045), does not call
`deepx_dxm1_bring_up()`, and has no Zephyr DEEPX backend.  Real DEEPX
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
