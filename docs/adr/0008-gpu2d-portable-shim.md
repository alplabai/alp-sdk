# 0008. GPU2D: portable shim under `<alp/gpu2d.h>` even for single-silicon

Status: Accepted
Date: 2026-05-14

## Context

The Alif Ensemble (AEN family) carries a TES **D/AVE 2D** GPU2D
accelerator -- a fixed-function 2D compositor (alpha blending,
rotation, scaling) for OLED / TFT display pipelines (`dave2d: true`
in the AEN SoC JSON, e.g. `metadata/socs/alif/ensemble/e7.json`).
None of the other E1M SoM families (V2N, V2N-M1, i.MX 93) populate
a peer accelerator.

> Correction (issue #24): earlier revisions of this ADR called the
> AEN 2D block "Mali-D71".  That is wrong -- Mali-D71 is an Arm
> *display processor* (a separate IP), not the AEN 2D compositor.
> The AEN GPU2D engine is TES D/AVE 2D (Alif's "GPU2D" marketing
> name), driven by the `d2_*` API.  All references below are
> corrected to D/AVE 2D.

The 2026-05-12 AEN feature audit flagged GPU2D as the headline
gap: the SDK's `<alp/display.h>` + `<alp/gui.h>` surfaces had
no portable way for an application to request alpha-blended
composition + scaling.  Apps that wanted these on AEN had to
drop down to Alif's vendor SDK directly, breaking the
"write once, run on any E1M module" promise.

Two ways to fix:

1. **Skip the SDK abstraction**: document that GPU2D is
   AEN-only and let customers link Alif's vendor library
   directly.  Cleaner for the SDK; abandons the portability
   contract for this class of feature.
2. **Expose a portable surface anyway**: ship
   `<alp/gpu2d.h>` with a generic open/blit/compose API.  The
   AEN backend wires it to D/AVE 2D; every other SoM is served by
   a portable software fallback.  Maintains the contract; one
   silicon family hardware-accelerates it today.

## Decision

**Ship `<alp/gpu2d.h>` as a portable shim** even though only
AEN populates it on v0.5.  Surface marked `[ABI-EXPERIMENTAL]`
since the API may need to adjust when a second silicon family
(future i.MX 93 GPU2D variant?) lands.

API shape:

```c
alp_gpu2d_t *alp_gpu2d_open(const alp_gpu2d_config_t *cfg);
alp_status_t alp_gpu2d_blit(alp_gpu2d_t *ctx,
                            const alp_gpu2d_surface_t *src,
                            alp_gpu2d_surface_t       *dst,
                            const alp_gpu2d_blit_op_t *op);
alp_status_t alp_gpu2d_compose(alp_gpu2d_t *ctx,
                               const alp_gpu2d_layer_t *layers,
                               size_t                   n_layers,
                               alp_gpu2d_surface_t     *dst);
void         alp_gpu2d_close(alp_gpu2d_t *ctx);
```

Backends (as implemented per issue #24):

- **AEN** (`src/backends/gpu2d/alif_dave2d.c`) -- the D/AVE 2D real
  backend, one registry entry per `alif:ensemble:e6` / `e7` / `e8`
  (the SKUs whose SoC JSON sets `dave2d: true`) at priority 100,
  driven through the documented `d2_*` API of the proprietary
  Alif D/AVE 2D pack (build-time pull only).  Bench-unverified at
  authoring time.  Backend selection is per-SoC-exclusive (no
  per-op fallback in the dispatcher), so blend modes the engine
  cannot express single-pass (ADDITIVE / MULTIPLY) are delegated
  to the software path by the backend itself, via the internal
  `alp_gpu2d_sw_ops()` hook -- the "write once" contract holds
  op-by-op, not just SoM-by-SoM.
- **V2N / V2N-M1 / i.MX 93 / native_sim / other**
  (`src/backends/gpu2d/sw_fallback.c`) -- the portable software
  fallback, wildcard `"*"` at priority 0.  It does the *real* CPU
  fill / blit / blend rather than returning `ALP_ERR_NOSUPPORT`, so
  `alp_gpu2d_open()` succeeds everywhere and callers get working 2D
  ops on SoMs with no on-die 2D engine.

> Note: the original ADR planned a NOSUPPORT-only stub on non-AEN
> SoMs.  Issue #24 superseded that with a real software fallback --
> a portable CPU 2D path is cheap and keeps the "write once" contract
> meaningfully (the op works, just slower), instead of forcing every
> non-AEN caller down a hand-written fallback.

## Consequences

### Positive

- "Write once, run on any E1M module" contract holds: app code
  that asks for GPU2D acceleration works on AEN, falls back
  cleanly elsewhere.
- AEN customers don't break the SDK abstraction when they want
  to use the silicon they paid for.
- Future silicon with a real 2D engine (a V2N-M1 GPU2D if one ever
  ships, or an i.MX PXP/g2d backend) plugs in behind the same
  surface at priority 100, above the software fallback.

### Negative

- One header with one backend.  Some SDK consumers will see
  this and ask "why is there a portable API for AEN-only
  silicon?"  Answer in this ADR + the header doc.
- Surface stays `[ABI-EXPERIMENTAL]` past v1.0 -- second-silicon
  feedback may force API tweaks.  Promotion to `[ABI-STABLE]`
  needs at least one non-AEN backend exercising the API.

### Neutral

- Surface designed with portability in mind: no D/AVE 2D
  features bleed into the public types.  A future i.MX PXP or
  other-vendor 2D backend would extend, not rewrite.

### i.MX 93 has no Vivante GPU (issue #24 premise was wrong)

Issue #24 framed a second real backend as "Vivante GC328 on
i.MX 93".  That premise is incorrect: the **i.MX 93 has no Vivante
GPU at all** -- its 2D engine is NXP's **PXP** (Pixel Pipeline), and
its only "GPU" is the separate-die-class options on i.MX 8/9 lines
(g2d on i.MX 8 via the GC-series Vivante core).  There is also no
`imx93` board in alp-sdk today.  So **no NXP 2D backend ships here**
and none is stubbed; an i.MX 93 PXP backend (or an i.MX 8 g2d
backend) is a clean future follow-up that would register at
priority 100 against that SoM's silicon_ref, above the software
fallback.  Do not re-introduce a "Vivante on i.MX 93" backend.

## Alternatives considered

- **AEN-only doc-stance** (option 1 above) -- abandons
  portability for this feature class.  Rejected: the SDK's
  whole identity is portability; carving exceptions weakens
  it.
- **Vendor-specific shim under `chips/`** -- the D/AVE 2D
  driver lives in chips/, callers opt in via `board.yaml`.
  Rejected: chips/ is for opt-in hardware drivers (sensors,
  PMICs, RTCs).  GPU2D is a SoC-integrated accelerator class,
  not an opt-in peripheral.
- **Surface inside `<alp/display.h>`** as a sub-API.
  Rejected: display.h is the framebuffer abstraction; mixing
  in compositor primitives blurs the boundary + makes
  display.h a giant header for the small set of consumers
  using GPU2D.

## See also

- [`<alp/gpu2d.h>`](../../include/alp/gpu2d.h) — the public API.
- [`src/backends/gpu2d/sw_fallback.c`](../../src/backends/gpu2d/sw_fallback.c)
  — the portable software 2D fallback (priority 0).
- [`src/backends/gpu2d/alif_dave2d.c`](../../src/backends/gpu2d/alif_dave2d.c)
  — the AEN D/AVE 2D real backend (priority 100, bench-unverified).
- [`docs/soms/aen.md`](../soms/aen.md) — AEN-family SoM support
  overview for SDK-visible accelerator blocks.
- [`docs/abi-markers.md`](../abi-markers.md) — explains why
  `<alp/gpu2d.h>` carries `[ABI-EXPERIMENTAL]` for v1.0.
