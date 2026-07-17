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

1. **PCIe mux + DEEPX power rail bring-up.**  The V2N
   supervisor's [`v2n_power_mgmt.c`](../../../src/zephyr/v2n_power_mgmt.c)
   module (landed in §C.28) responds to the board's
   `DEEPX_PWR_EN_REQ` rising edge on `P65`, brings up the
   DA9292 CH2 = 0.75 V DEEPX rail, then drives
   `DEEPX_CORE_0P75_EN` (`P64`) high.  This happens
   automatically on SYS_INIT before this example's `main()`
   runs.
2. **PCIe mux + `M1_RESET` release.**  The
   [`chips/deepx_dxm1/`](../../../chips/deepx_dxm1/) host
   driver wraps the PI3DBS12212 PCIe mux routing + the
   Renesas `PA6` `M1_RESET` release into one
   `deepx_dxm1_bring_up()` call.
3. **Inference handle open via the portable `<alp/inference.h>`
   surface.**  `backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1` +
   `format = ALP_INFERENCE_MODEL_DXNN`.  Under the hood the
   SDK dispatches to the DEEPX `dx_rt` runtime via header
   bindings; the runtime itself is pulled in by the customer
   per the licence story above.
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

```bash
# Pull in the DEEPX runtime per the customer-licence story
# documented in docs/vendor-partnerships.md §DEEPX.
git clone https://github.com/DEEPX-AI/dx_rt modules/dx_rt
git clone https://github.com/DEEPX-AI/dx_rt_npu_linux_driver modules/dx_rt_driver

west alp-build -b alp_e1m_v2m101_m33_sm/r9a09g056n48gbg/cm33 examples/v2n/v2n-m1-deepx-inference
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
