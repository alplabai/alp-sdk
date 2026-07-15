# AEN accelerator backends — integration design

Status: Draft (design only — no real bodies land here)
Date: 2026-06-14

Design notes for the Alif Ensemble (AEN-family) on-die accelerator
surfaces flagged by the 2026-05-12 AEN feature audit as
NEEDS-PORTABLE-SURFACE / vendor-ext gaps:

1. **GPU2D (D/AVE 2D)** — fixed-function 2D blit / fill / blend.
2. **VeriSilicon ISP Pico (`vsi,isp-pico`)** — image-signal-processor
   (3A, per-channel gain, lens-shading correction).
3. **OSPI SecAES** — inline-AES on-the-fly XIP encryption for
   external OctalSPI flash.
4. **aiPM power management** — DVFS / run-profiles / PMIC sequencer
   across the A32 + M55 + NPU domains (**different status — see §4**).

Surfaces 1–3 ship a header + a stub today; each stub returns
`ALP_ERR_NOSUPPORT` after the standard surface / vendor-handle
validation.  For GPU2D and SecAES (§1, §3) the real body is silicon +
Alif-HAL-pack gated — no SoM in scope ships the matching D/AVE 2D or
SecAES pack yet.  ISP (§2) is different: its HAL pack (`isp_wrapper`)
is already vendored, but the real body is blocked by four independent
reasons plus a deliberate AWB hold — see §2 below.  This document
captures the integration design (DMA / coherency / dispatch / keying)
the real backends must honour when they land — it is ADR-adjacent
reference, not an implementation.

The aiPM power surface (§4) is the exception: it already has a working
generic backend, so the design question there is *what the vendor
backend adds and why no AEN-specific stub lands pre-silicon*, not how
to replace a NOSUPPORT stub.

For the per-surface verification rows see the v0.5 section of
[`docs/test-plan.md`](test-plan.md).  For the GPU2D portability
decision see [ADR 0008](adr/0008-gpu2d-portable-shim.md).

---

## Silicon scope — which E-part has what

The single source of truth for AEN-family capabilities is the SoC
JSON under `metadata/socs/alif/ensemble/`.  The three surfaces here
do **not** all populate on the same parts, and the distinction
matters because the E1M-AEN701 module SKU is built on the **E7**
part:

| Surface                | Populates on        | E7 (E1M-AEN701)?                       |
|------------------------|---------------------|---------------------------------------|
| GPU2D (D/AVE 2D)       | E6 / E7 / E8 only   | **Yes** — `gpu2d` + `dave2d` true in `e7.json` |
| ISP Pico (`vsi,isp-pico`) | E4 / E6 / E8 only | **No** — no `isp` key in `e7.json` |
| OSPI SecAES (vendor)   | E4 / E6 / E8 only   | **No** SecAES fabric (see note below) |

This table is hardware metadata, not a current backend-support matrix:
the in-repo ISP-Pico backend registration is E8-only until E4 / E6
have explicit backend rows and board validation.

> **E7 inline-AES nuance.**  `e7.json` carries `inline_aes: true` and
> the OctalSPI interface lists `"AES inline"` — that is the *base*
> OSPI on-the-fly AES path the portable
> `alp_storage_configure_inline_aes` targets.  The finer-grained
> **vendor SecAES surface** in `<alp/ext/alif/storage.h>` (key
> provisioning + engine status read-back) is gated to E4 / E6 / E8;
> E3 / E5 / E7 lack that hardened SecAES block and return
> `ALP_ERR_NOSUPPORT` (and `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` for a
> non-Alif handle).  Keep the two distinct: the portable surface is
> capability-bit-gated, the vendor surface is silicon-list-gated.

The practical consequence for the EdgeAI vision example
(`examples/aen/edgeai-vision-aen/`, an **E8 target** — `som.sku:
E1M-AEN801`): the in-repo ISP-Pico backend is scoped to E8 today, so
the example may eventually offload debayer / format-convert / 3A to
it — but none of the three works yet, for two unrelated reasons.
Debayer and format-convert are blocked at the Zephyr driver layer:
`zephyr/drivers/video/isp_pico.c` (the same `vsi,isp-pico` node this
backend targets) links against a newer `hal_alif` libisp wrapper than
the one vendored locally, so it compiles but will not link (see that
file's HAL_ALIF VERSION MISMATCH note) — even though the MPI calls
debayer/format-convert need, `VSI_MPI_ISP_SetDmscAttr` and
`VSI_MPI_ISP_SetScaleAttr`, are defined in the vendored archive. 3A is
blocked for a different, independent reason: AE is declared in the
vendored isp_wrapper headers but undefined in the archive; AF and LSC
are absent from that archive outright (no header, no symbol); and,
for an additional, independent reason, the per-channel gain path is
**contract-absent** — see the per-entry detail in
`src/backends/ext/alif/camera.c`. So today the example must **not**
rely on the ISP: it configures the camera sensor to emit the model's
pixel format directly and does crop / resize / normalisation on the
M55-HP with CMSIS-DSP — exactly the data flow in that example's
`docs/pipeline.md`.

---

## 1. GPU2D (D/AVE 2D) backend

**Portable surface:** `<alp/gpu2d.h>` — `alp_gpu2d_open` /
`alp_gpu2d_fill_rect` / `alp_gpu2d_blit` / `alp_gpu2d_blend` /
`alp_gpu2d_close`.
**Portable fallback:** `src/backends/gpu2d/sw_fallback.c` (wildcard
`"*"`, priority 0, vendor `"sw"`) — a REAL pure-C CPU
fill/blit/blend, not a NOSUPPORT stub.  It replaced the original
`zephyr_stub.c` (issue #24).
**Real backend:** `src/backends/gpu2d/alif_dave2d.c` — Alif D/AVE 2D
HAL on the AEN family, registering a silicon-specific entry per
`alif:ensemble:e6` / `e7` / `e8` (the SKUs whose SoC JSON carries
`dave2d: true`) at priority 100, above the wildcard fallback
(tracking issue #24; bench-unverified).

### Dispatch + registration

The software fallback registers at priority 0 against `"*"` so every
build links `<alp/gpu2d.h>` cleanly and the dispatcher's
surface-validation pre-checks (NULL handle, zero dimensions, format
range, row-fits-stride) stay reachable on every SoC.  The real
D/AVE 2D backend registers against the concrete AEN silicon_refs at
priority 100; the registry then prefers it over the wildcard
fallback on those parts and leaves every other SoM (V2N — no on-die
2D block — i.MX 93, and bare-metal) on the fallback's REAL CPU
fill/blit/blend.  Selection is per-SoC-exclusive — there is no
per-op fallback in the dispatcher — so ops the engine cannot express
single-pass (the ADDITIVE / MULTIPLY blend modes) are delegated to
the software path by the D/AVE 2D backend itself, through the
internal `alp_gpu2d_sw_ops()` hook.

### DMA-queue semantics

D/AVE 2D is a display-list / command-queue engine, not a
register-poked blitter.  The real backend must:

- Build each `fill_rect` / `blit` / `blend` op into a D/AVE 2D
  display-list entry rather than issuing it synchronously.  The
  public API today is call-at-a-time (one op returns before the
  next), so the v0.5 surface assumes **submit-and-wait** per op:
  enqueue the op, flush the list, block on the engine's done IRQ /
  poll, then return.  This keeps the simple ordering contract the
  header documents while leaving room for a future batched-submit
  extension (TBD — would need an explicit flush entry point on the
  public API, out of scope until a real consumer needs it).
- Serialise issuance behind the driver mutex the header already
  documents — the singleton handle is reentrant only under that
  lock, and the HAL's display-list builder is not itself
  thread-safe.

### Coherency

The `alp_gpu2d_surface_t` `base` pointer is caller-owned framebuffer
memory; D/AVE 2D reads / writes it by DMA. The real backend owns the
cache maintenance the caller cannot see:

- **Before** the engine reads a source surface: clean (write-back)
  the source range from the CPU cache so the engine sees the
  caller's latest pixels.
- **After** the engine writes the destination surface: invalidate
  the destination range in the CPU cache so a subsequent CPU read
  (or a later op that reads it as a source) sees engine output, not
  stale cached lines.
- Surfaces in M55-HP TCM are not cached and need no maintenance;
  surfaces in SRAM banks do.  The backend keys cache ops off the
  surface address region, not off a per-surface flag (the public
  type carries no cache hint and must not grow one — that would leak
  a silicon detail into the portable struct).

`stride_bytes` lets the caller hand the engine a sub-rect of a
larger framebuffer without copying; the cache-maintenance range must
be computed per-row across the `h` rows actually touched, not over
the whole `base..base+stride*height` span, to avoid clobbering
neighbouring lines another context may own.

---

## 2. VeriSilicon ISP Pico (`vsi,isp-pico`) dispatch path

**Vendor surface:** `<alp/ext/alif/camera.h>` —
`alp_alif_camera_isp_3a_window_set` /
`alp_alif_camera_isp_gain_table_load` /
`alp_alif_camera_isp_lsc_lut_load`.
**Stub it replaces:** `src/backends/ext/alif/camera.c` (vendor-handle
gating today; bodies return `ALP_ERR_NOSUPPORT`).
**Real backend:** `alif_isp_pico` camera backend, priority 100,
registered against `alif:ensemble:e8` only.  E4 / E6 metadata exposes
ISP-capable silicon, but alp-sdk has no E4 / E6 backend registration
or board validation yet.

These are the finer-grained ISP knobs that do not fit the portable
`alp_camera_isp_config_t` (`alp_camera_configure_isp`): pixel-coordinate
rectangles and kibibyte-scale LUT pointers would force every
backend — including the ISP-Pico-less E3 / E5 / E7 parts — to carry
the equivalent fields, so they live in the vendor-ext header behind
the Alif-handle gate.

### Dispatch path

1. App opens the camera via the portable `alp_camera_open`; the
   dispatcher binds the `alif_isp_pico` backend when the silicon_ref
   is `alif:ensemble:e8`.
2. App includes `<alp/ext/alif/camera.h>` and calls the vendor knob.
   Each call re-checks the handle's backend vendor is `"alif"`
   (`_is_alif_backend`); a non-Alif handle returns
   `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` **before** any hardware touch.
3. The real body programs the ISP Pico statistics / gain / LSC
   registers through the HAL and returns `ALP_OK` (today: returns
   `ALP_ERR_NOSUPPORT` after argument validation).

### 3A / gain / LSC semantics

- **3A windows** (`isp_3a_window_set`): the ISP Pico carries a
  1024-cell weighted statistics grid per 3A loop (AE / AWB / AF).
  The portable rectangle selects which inclusive cell span feeds the
  loop; the API honours one rectangle per loop and a fresh call
  replaces the prior one. The backend maps the caller's
  pixel-coordinate rect onto the 1024-cell grid and writes the
  per-loop window registers.  Rectangle math is shared with the
  Renesas V2N N44 ISP surface (`alp_renesas_camera_rect_t` has the
  identical layout) so customers spanning V2N + AEN reuse it.
- **Per-channel gain LUT** (`isp_gain_table_load`): Q4.12 gain
  entries applied per Bayer channel (R / Gr / Gb / B) before AWB
  integration. **Reference-not-copy**: the caller's `table` buffer
  must stay live until close or the next load — the backend programs
  the engine to fetch from it, it does not snapshot.  The backend
  must therefore pin / cache-clean the table for the engine's DMA
  fetch with the same range discipline as the GPU2D source-surface
  rule above.
- **LSC MESH LUT** (`isp_lsc_lut_load`): a flattened 2D vignetting
  surface, Q4.12 per grid cell, same reference-not-copy + cache
  contract.

The validation envelope is already in the stub and must be preserved
verbatim by the real bodies (NULL handle / NULL buffer →
`ALP_ERR_INVAL`; non-Alif backend →
`ALP_ERR_NOT_PRESENT_ON_THIS_SOC`; out-of-range `len` →
`ALP_ERR_INVAL`).  Only the terminal `ALP_ERR_NOSUPPORT` is replaced
by real work.

---

## 3. OSPI SecAES inline-XIP encrypted flash

**Vendor surface:** `<alp/ext/alif/storage.h>` —
`alp_alif_storage_secaes_key_provision` /
`alp_alif_storage_secaes_get_status`.
**Stub it replaces:** `src/backends/ext/alif/storage.c` (vendor-handle
gating today; bodies return `ALP_ERR_NOSUPPORT`).
**Portable peer:** `alp_storage_configure_inline_aes`
(`<alp/storage.h>`) — the base on-the-fly AES path; the vendor
surface adds hardware-slot key provisioning + engine status
read-back the portable struct can't express.

### Keying flow

The SecAES block binds an AES key into a hardware slot so the key
material never traces back to host RAM after the call returns:

1. Caller provisions the key:
   `alp_alif_storage_secaes_key_provision(s, key, key_bytes)` with
   `key_bytes ∈ {16, 24, 32}` (AES-128 / 192 / 256).  After return,
   the caller should zeroise its own key buffer — the engine holds
   the only live copy.
2. Caller sets cipher mode + IV / tweak via the portable
   `alp_storage_configure_inline_aes` (CTR or XTS); the same hardware
   slot honours it.  XTS at flash-block granularity is the standard
   choice for stored-data / XIP encryption.
3. Caller confirms the engine is live before trusting encrypted XIP:
   `alp_alif_storage_secaes_get_status(s, &flags)` must report
   `ALP_ALIF_STORAGE_SECAES_STATUS_ENGAGED` (and not
   `KEY_LOAD_ERR` / `BUS_ERR`).  In a secure-boot flow the
   application **aborts** if the encryption fabric is not confirmed
   live — this is the load-bearing reason the status read-back is a
   separate call rather than an implicit side effect of provisioning.

### Inline-XIP semantics

Once provisioned + engaged, the OctalSPI controller encrypts on
write and decrypts on read transparently between the host bus and
the external chip: XIP code executes plaintext from the host's view
while the flash stores ciphertext.  No per-access API: existing
`alp_storage_read` / `alp_storage_write` and CPU instruction fetch
all flow through the engaged engine.  The backend must ensure the
engine is armed before the first XIP fetch from an encrypted region;
provisioning + configure complete synchronously so the ordering is
the caller's responsibility, documented on the surface.

The status read-back must reflect real engine state once the HAL
lands; the stub currently seeds `..._STATUS_IDLE` and still returns
`ALP_ERR_NOSUPPORT`, so callers cannot mistake the placeholder for a
live engine.

---

## 4. aiPM power management

**Portable surface:** `<alp/power.h>` — `alp_power_open` /
`alp_power_configure_wake_source` / `alp_power_request_sleep` /
`alp_power_close`.
**Backend today:** `src/backends/power/zephyr_pm_policy.c` — wildcard
`"*"`, priority 100, vendor `"zephyr"` — **already serves AEN**: `open`
/ `configure_wake_source` succeed and `request_sleep` drives the Zephyr
PM policy.  `src/backends/power/zephyr_stub.c` (`"*"`, priority 0) is
the link-floor beneath it.
**Real backend:** an `alif_aipm` power backend registered against
`alif:ensemble:e3`…`e8` at priority **101** (above the wildcard
policy), landing with the Alif aiPM / PMIC-sequencer HAL.

### Why no AEN-specific power stub lands pre-silicon

Unlike GPU2D / ISP / SecAES — which have *no* generic backend, so a
NOSUPPORT stub is strictly additive — power already has a working
wildcard backend.  Adding an AEN-specific power backend now would be a
**regression, not scaffolding**:

- At priority 100 it would tie `zephyr_pm_policy` (undefined ordering);
  at 101 it would *override* the working policy with an incomplete body.
- A NOSUPPORT-returning `open` would break the "a handle is always
  returned, sleep is the gate" contract that `zephyr_stub` /
  `zephyr_pm_policy` honour, so setup code that opens a power handle on
  AEN would start failing.
- An `alif:ensemble:*` backend is not built or selected in the
  `native_sim` twister gate (wrong silicon_ref), so it would be
  untested code until silicon.

So the AEN power backend lands **with** the HAL + silicon, not before —
this is the one audit gap that deliberately does *not* get a stub here.

### What the real aiPM backend owns

- **Run profiles / DVFS** across the A32 cluster, the M55-HP / M55-HE,
  and the NPU domains — the aiPM headline feature.
- **PMIC / rail sequencer**: rail enable / disable ordering, honouring
  the cold-boot constraint in `e8.json` errata **ER004** (VDD_MAIN /
  VDD_BUCK must ramp monotonically and never dip below 1.65 V once
  reached — an external reset supervisor drives `POR_N` until then) and
  the STOP-mode current targets (`power.stop_ua` 1.3 µA; errata
  **ER003** STOP current on A0 silicon).
- **Wake sources + retention**: map the `alp_power` wake bitmap onto the
  AEN RTC / LPGPIO / comparator wakes, accounting for the RTC quirks in
  errata **ER001 / ER002** (clock-prescaler reset / LFRC drift while
  `POR_N` is asserted).

These are all bench-confirmable only on the physical SoM; the design
here is what the backend must implement, not a stub.

---

## Open items (TBD until the HAL packs land)

- **D/AVE 2D batched submit.**  The v0.5 public API is submit-and-wait
  per op; a batched display-list flush would need a new public entry
  point.  Deferred until a real consumer (e.g. an LVGL/D/AVE 2D
  integration) needs it.
- **ISP Pico vs portable ISP overlap.**  How much of the portable
  `alp_camera_configure_isp` the ISP Pico backend serves vs. the
  vendor knobs is settled when the HAL pack lands; both surfaces are
  `[ABI-EXPERIMENTAL]` precisely so that boundary can move.
- **SecAES key-slot count + lifecycle.**  Number of slots, whether
  re-provisioning replaces or allocates, and the wipe-on-tamper
  contract are HAL-defined and recorded here when the pack is in
  scope.

---

## See also

- [`docs/test-plan.md`](test-plan.md) — v0.5 verification rows for
  all three surfaces (all `⏳ untested`; GPU2D + SecAES HAL-pack-gated,
  ISP blocked for the different reasons in §2 above).
- [ADR 0008](adr/0008-gpu2d-portable-shim.md) — why GPU2D ships as a
  portable shim even on single-silicon.
- [`include/alp/gpu2d.h`](../include/alp/gpu2d.h) /
  [`src/backends/gpu2d/sw_fallback.c`](../src/backends/gpu2d/sw_fallback.c) /
  [`src/backends/gpu2d/alif_dave2d.c`](../src/backends/gpu2d/alif_dave2d.c)
  — GPU2D surface + software fallback + D/AVE 2D real backend.
- [`include/alp/ext/alif/camera.h`](../include/alp/ext/alif/camera.h) /
  [`src/backends/ext/alif/camera.c`](../src/backends/ext/alif/camera.c)
  — ISP Pico (`vsi,isp-pico`) vendor surface + stub.
- [`include/alp/ext/alif/storage.h`](../include/alp/ext/alif/storage.h) /
  [`src/backends/ext/alif/storage.c`](../src/backends/ext/alif/storage.c)
  — SecAES vendor surface + stub.
- [`include/alp/storage.h`](../include/alp/storage.h) — the portable
  inline-AES peer (`alp_storage_configure_inline_aes`).
- [`include/alp/power.h`](../include/alp/power.h) /
  [`src/backends/power/zephyr_pm_policy.c`](../src/backends/power/zephyr_pm_policy.c)
  — aiPM portable surface + the wildcard backend that already serves AEN.
- [`metadata/socs/alif/ensemble/e7.json`](../metadata/socs/alif/ensemble/e7.json)
  — authoritative E7 capability set (no ISP Pico).
