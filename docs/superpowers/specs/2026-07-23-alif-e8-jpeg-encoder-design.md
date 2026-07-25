# Design: Alif Ensemble E8 (E1M-AEN801) JPEG encoder surface

**Date:** 2026-07-23
**Status:** Draft for review
**Scope:** New portable `<alp/jpeg.h>` JPEG-**encoder** surface for alp-sdk, hardware-accelerated
on the E1M-AEN801 (Alif AE822 / Ensemble E8) Hantro VC9000E block, with a software baseline
fallback for every other SoM. **Encode only** — the E8 has no hardware JPEG decoder.

---

## 1. Motivation

`metadata/socs/alif/ensemble/e8.json` declares `peripherals.jpeg_encoder: 1` and
`optional_features.jpeg_encoder: true` for the AEN801-tied FBGA194 variant, with the prose note
"E8 ships an ISP and JPEG encoder (E3/E7 do not)." Today that capability is **metadata-only**:
there is no JPEG driver, backend, or public API anywhere in `src/` or `zephyr/`. This design adds
the surface end to end so a customer can encode a captured frame to JPEG through a portable API,
offloaded to hardware on AEN and done in software elsewhere.

This is the greenfield half of the imaging gap. The ISP half (`alif_isp_pico.c` param-upload TBD)
is tracked separately and is **out of scope** here.

## 2. Silicon facts (verbatim — do not round)

Source: `Alif_E8_Datasheet_v1.0.pdf` §3.19.4 pp.85–86; `Confidential-Alif_E8_HWRM_v0.3.pdf` §17.4
p.1768; register map Table 17-54 p.1771; IRQs Table 17-52 p.1770.

- **Block:** JPEG Encoder, VeriSilicon **Hantro VC9000E**.
- **Base address:** `0x4904_4000`. Power domain **PD-6**. **128-bit AXI master**.
- **Clock:** `JPEG_CLK = SYST_ACLK`, up to **400 MHz**. Clock-enable helper "Enable JPEG clock" in
  the System Control block.
- **IRQs (Level-triggered), M55-HP / M55-HE numbers:**
  - `JPEG_XINT_IRQ` → M55HP_IRQS[360] / M55HE_IRQS[360]
  - `JPEG_XINT_NORM_IRQ` → 361 / 361
  - `JPEG_XINT_ABN_IRQ` → 362 / 362
  - `JPEG_IDLE_IRQ` (a.k.a. `JPEG_XINT_IDLE_IRQ`) → 363 / 363
- **Standards:** JPEG ISO/IEC 10918-1, ITU-T T.81 baseline (Huffman, interleaved YUV420);
  MJPEG (T.81 Annex H) in AVI container.
- **Subsampling / formats:** encode color space YUV **4:0:0, 4:2:0, 4:2:2**; input YUV400/420/422,
  raster-scan or tiled layout; on-chip CSC **422→420**; encode bit depth **8 bits**; output step
  size 16 px; slices mode (N macroblock-rows/slice); rate control (target bits/picture).
- **Dimensions:** max frame width **16K**, min resolution **32×32**, up to **2MP @ 200 FPS**.
- **No hardware JPEG decoder** exists on E8 (confirmed absent, full-doc search). Decode, if ever
  needed, is software and is not part of this surface.
- **Errata:** none touching JPEG (`AERR0012` v2.0 has only ER001–ER004, all non-imaging).
- **Register map (Table 17-54):** `JPEG_SWREG0 0x0` (R), `JPEG_SWREG1 0x4`, `JPEG_SWREG2 0x8`,
  `JPEG_SWREG3 0xC`, `JPEG_SWREG4 0x10`, `JPEG_SWREG5 0x14`, `JPEG_SWREG8 0x20`, `JPEG_SWREG9 0x24`,
  `JPEG_SWREG12 0x30`, … (VeriSilicon `swregN` style). Per-bit programming detail is a placeholder
  ("under development") in HWRM §17.4.3 — we do **not** program these registers directly (see §4).

## 3. Source & license strategy (decisive)

The Alif **CMSIS DFP** (`alifsemi/alif_ensemble-cmsis-dfp`) ships `Driver_JPEG.{h,c}` +
`drivers/source/jpeg_hantro_vc9000e.c`, but under the **Alif Semiconductor Software License
Agreement** — clause 4 restricts use to Alif silicon, clause 5 forbids copyleft. That license is
**incompatible with the public alp-sdk repo. DFP source is NOT vendored into this repo.**

Instead we consume the driver the way the ISP path already does:

- **hal_alif** (`alifsemi/hal_alif`, pinned in `west.yml` at `modules/hal/alif`, Apache-2.0 wrapper)
  ships `drivers/jpeg/` — prebuilt `libjpeg_hantro_vc9000e_sw_{gcc,armclang,llvm}.a` blobs +
  public header `jpeg_hantro_vc9000e_sw.h` + Kconfig. The Alif-IP code lives in the **fetched
  module**, never in our tree — exactly like `drivers/isp/libisp_gcc.a`.
- The DFP `Driver_JPEG.c` is used only as a **read-only programming reference** for how to drive the
  hantro SW library API (since the HWRM register narrative is a placeholder). We do not copy it.

Net: the public alp-sdk repo gains only its own portable API, dispatch, a thin Alif backend that
calls the hal_alif lib, and a from-scratch permissively-licensed software fallback.

## 4. Architecture

Mirrors the established portable-peripheral pattern (camera / i2c precedent):
`public header → dispatch (backend-registry class) → per-silicon backends selected by `silicon_ref``.

```
include/alp/jpeg.h                     public API  [ABI-EXPERIMENTAL]
  src/jpeg_dispatch.c                  ALP_BACKEND_DEFINE_CLASS(jpeg) + alp_backend_select + slot pool
  src/backends/jpeg/jpeg_ops.h         vtable (alp_jpeg_ops) + struct alp_jpeg handle
  src/backends/jpeg/alif_hantro.c      silicon_ref="alif:ensemble:e8", prio 100 -> hal_alif VC9000E lib
  src/backends/jpeg/sw_baseline.c      silicon_ref="*", prio 50 -> compact baseline encoder (fallback)
  src/backends/jpeg/zephyr_stub.c      silicon_ref="*", prio 0  -> ALP_ERR_NOT_IMPLEMENTED (last resort)
  src/common/stub/stub_jpeg.c          native_sim host stub
```

### 4.1 Public API (`include/alp/jpeg.h`)

Encode-only, one-shot frame encode plus a capabilities query. Doxygen on every public symbol
(house style). `[ABI-EXPERIMENTAL]` marker (camera.h precedent).

```c
typedef enum {
    ALP_JPEG_SUBSAMPLE_400,   /* monochrome / Y-only  */
    ALP_JPEG_SUBSAMPLE_420,   /* 4:2:0                 */
    ALP_JPEG_SUBSAMPLE_422,   /* 4:2:2                 */
} alp_jpeg_subsample_t;

typedef struct {
    uint16_t width;                 /* 32..16384 (HW); >= 8 (SW) */
    uint16_t height;
    alp_jpeg_subsample_t subsample;
    uint8_t  quality;               /* 1..100                    */
    /* input planes: YUV, 8-bit, raster layout (tiled = HW-only, future) */
    const void *y_plane; uint32_t y_stride;
    const void *u_plane; uint32_t u_stride;   /* NULL for _400 */
    const void *v_plane; uint32_t v_stride;
} alp_jpeg_encode_req_t;

alp_jpeg_t *alp_jpeg_open(const alp_jpeg_config_t *cfg, alp_err_t *err);
alp_err_t   alp_jpeg_encode(alp_jpeg_t *h, const alp_jpeg_encode_req_t *req,
                            void *out_buf, size_t out_cap, size_t *out_len);
alp_err_t   alp_jpeg_capabilities(alp_jpeg_t *h, alp_jpeg_caps_t *caps);
alp_err_t   alp_jpeg_close(alp_jpeg_t *h);
```

`alp_jpeg_caps_t` reports: `hw_accelerated` (bool), supported subsamplings, max/min dimensions,
`mjpeg_supported` (Alif HW: yes; SW: no). Callers gate optional behavior on caps, never on SoM name.

### 4.2 Alif hardware backend (`alif_hantro.c`)

- `ALP_BACKEND_REGISTER(jpeg, alif_hantro, { .silicon_ref = "alif:ensemble:e8", .priority = 100, ... })`.
- Thin adapter over hal_alif's `jpeg_hantro_vc9000e_sw.h` API — configure dimensions/subsample/quality,
  submit planes, wait on completion IRQ, return encoded length. No direct `JPEG_SWREGn` poking.
- Gated `CONFIG_ALP_SDK_JPEG_ALIF_HANTRO` (`depends on` the hal_alif jpeg Kconfig), default **n**
  (Tier-2 opt-in, matching `CONFIG_ALP_SDK_CAMERA_ALIF_ISP`).
- `caps.mjpeg_supported = true`, `max width 16384`, `2MP`.

### 4.3 Software fallback backend (`sw_baseline.c`)

- `silicon_ref="*"`, prio 50 — selected on any SoM without a higher-priority JPEG backend (e.g. V2N,
  which has no JPEG hardware).
- Compact **baseline-sequential** encoder (public-domain, TooJpeg-class ~400 LOC, vendored with its
  permissive notice). **Ceiling, documented in-source:** baseline sequential only, YUV 4:2:0 and
  4:0:0, no MJPEG, no rate control, no tiled input. `caps.hw_accelerated=false`,
  `caps.mjpeg_supported=false`.
- `ponytail:` comment naming the ceiling + upgrade path (swap for libjpeg-turbo only if a customer
  measurably needs progressive/4:2:2/throughput in software).

### 4.4 Metadata / capability wiring

- `metadata/socs/alif/ensemble/e8.json` — `jpeg_encoder` already declared; **no change needed**.
- `metadata/catalog.json` — add a `jpeg` class entry (header path + public symbols), mirroring the
  camera block.
- `metadata/e1m_modules/E1M-AEN801.yaml` — JPEG is an on-die block with no E1M-connector pins;
  **no pad_routes change**.

## 5. Error handling

- `alp_jpeg_open` on a SoM with no registered JPEG backend → the dispatch returns the stub, whose ops
  yield `ALP_ERR_NOT_IMPLEMENTED`; `caps` still answers so callers can degrade gracefully.
- `out_cap` too small → `ALP_ERR_NO_SPACE`, `*out_len` set to the required size when known.
- Unsupported subsample on the SW backend (e.g. `_422`) → `ALP_ERR_NOT_SUPPORTED`, not a silent
  re-sample.
- HW timeout / abnormal IRQ (`JPEG_XINT_ABN_IRQ`) → `ALP_ERR_IO`; handle left closeable.
- Lifecycle guards + slot-claim handle pool reused from the dispatch precedent (`alp_slot_claim`).

## 6. Testing

- `tests/unit/jpeg_registry/` — backend selection: AEN resolves `alif_hantro`, `*` resolves
  `sw_baseline`, capability flags correct. (native_sim, no hardware.)
- SW-backend correctness self-check: encode a known 16×16 YUV420 pattern, assert the output is a
  valid baseline JPEG (SOI/EOI markers, decodable by a reference decoder in the test host).
- `examples/aen/aen-jpeg-regcheck/` — bring-up reg-check example mirroring `aen-isp-regcheck`
  (build_only + HiL-gated); confirms clock-enable + block ID read on real silicon.
- Emit-parity fixture entry if the example is added to the emit set.
- **Bench-before-merge:** HW backend validated on real AEN801 silicon (encode a real captured frame,
  confirm the JPEG opens in a standard viewer) before the HW path is called done. SW path merges on
  native_sim evidence alone.

## 7. Out of scope

- ISP param-upload finish (`alif_isp_pico.c` TBD) — separate thread.
- JPEG **decode** (no E8 hardware; not added in software here).
- MJPEG/AVI container muxing (HW can produce MJPEG frames; container assembly is app-level).
- Yocto / A32 JPEG path (this surface targets the M55 / Zephyr cores, matching the camera stack).
- Tiled input layout, progressive JPEG, 4:2:2 in software.

## 8. Lockstep checklist (propagating-code-changes)

`include/alp/jpeg.h` · `src/jpeg_dispatch.c` · `src/backends/jpeg/{jpeg_ops.h,alif_hantro.c,
sw_baseline.c,zephyr_stub.c}` · `src/common/stub/stub_jpeg.c` · `zephyr/CMakeLists.txt` (sources +
`zephyr_library_sources_ifdef`) · `zephyr/kconfigs/*.kconfig` (`CONFIG_ALP_SDK_JPEG_*`) ·
`metadata/catalog.json` · `tests/unit/jpeg_registry/` · `examples/aen/aen-jpeg-regcheck/` ·
`docs/abi-markers.md` (ABI-EXPERIMENTAL entry) · docs surface (CLI/getting-started if a JPEG example
is referenced).
