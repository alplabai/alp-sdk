# 0008. GPU2D: portable shim under `<alp/gpu2d.h>` even for single-silicon

Status: Accepted
Date: 2026-05-14

## Context

The Alif Ensemble (AEN family) carries a Mali-D71 GPU2D
accelerator -- a fixed-function 2D compositor (alpha blending,
rotation, scaling) for OLED / TFT display pipelines.  None of
the other E1M SoM families (V2N, V2N-M1, i.MX 93) populate a
peer accelerator.

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
   AEN backend wires it to Mali-D71; every other backend
   returns `ALP_ERR_NOSUPPORT` cleanly.  Maintains the
   contract; one silicon family populates it today.

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

Backends:

- **AEN-Zephyr** (`src/zephyr/gpu2d_zephyr.c`) -- wires to
  Mali-D71 via Alif's vendor SDK.
- **V2N / V2N-M1 / i.MX 93 / other** -- return
  `ALP_ERR_NOSUPPORT` from `alp_gpu2d_open`.  Callers use
  `alp_last_error()` to diagnose + fall back to CPU
  composition.

## Consequences

### Positive

- "Write once, run on any E1M module" contract holds: app code
  that asks for GPU2D acceleration works on AEN, falls back
  cleanly elsewhere.
- AEN customers don't break the SDK abstraction when they want
  to use the silicon they paid for.
- Future silicon (i.MX 93 GPU2D, V2N-M1 GPU2D if it ever
  ships) plugs in behind the same surface.

### Negative

- One header with one backend.  Some SDK consumers will see
  this and ask "why is there a portable API for AEN-only
  silicon?"  Answer in this ADR + the header doc.
- Surface stays `[ABI-EXPERIMENTAL]` past v1.0 -- second-silicon
  feedback may force API tweaks.  Promotion to `[ABI-STABLE]`
  needs at least one non-AEN backend exercising the API.

### Neutral

- Surface designed with portability in mind: no Mali-D71
  features bleed into the public types.  An i.MX 93 or
  Mali-Cxx backend that ships future would extend, not
  rewrite.

## Alternatives considered

- **AEN-only doc-stance** (option 1 above) -- abandons
  portability for this feature class.  Rejected: the SDK's
  whole identity is portability; carving exceptions weakens
  it.
- **Vendor-specific shim under `chips/`** -- the Mali-D71
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
- [`src/zephyr/gpu2d_zephyr.c`](../../src/zephyr/gpu2d_zephyr.c)
  — AEN-Zephyr backend.
- Internal AEN feature audit (private repo) — the report that
  flagged GPU2D as the headline gap.
- [`docs/abi-markers.md`](../abi-markers.md) — explains why
  `<alp/gpu2d.h>` carries `[ABI-EXPERIMENTAL]` for v1.0.
