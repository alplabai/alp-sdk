# 0007. Wave-2 DSP: pipeline stages, not standalone primitives

Status: Accepted
Date: 2026-05-14

## Context

The v0.5 wave-2 work introduced FFT, FAC (finite arithmetic
coder), FIR / IIR filters, and windowing primitives.  Two ways to
expose them:

1. **Standalone library style.**  Each primitive is its own
   open/configure/apply API: `alp_fft_open()`, `alp_fir_open()`,
   `alp_window_open()`, plus glue code to wire them together
   when the customer wants a multi-stage pipeline.
2. **ADC-pipeline stages.**  The primitives are *stages* of an
   ADC stream chain (`alp_adc_filter_t` /
   `alp_adc_spectrum_t`) plus a composable in-RAM chain
   (`alp_dsp_chain_t`) for callers who don't need an ADC source.

The maintainer flagged early that the v0.5 hardware target (the
GD32G553 supervisor's FFT / FAC blocks) only makes sense as
**part of an ADC pipeline**: the FFT runs on bins extracted
from a streamed ADC capture; standalone "FFT this in-RAM
buffer" misses the hardware acceleration opportunity entirely.

## Decision

Wave-2 DSP ships as **two surfaces both rooted in the ADC pipeline**:

1. **`<alp/adc.h>` ADC-pipeline types** -- `alp_adc_filter_t`
   (a stream → filter → out flow) and `alp_adc_spectrum_t`
   (a stream → FFT → bins flow).  These are the load-bearing
   APIs for SoMs whose backend offloads DSP to silicon
   (GD32G553 FFT/FAC).  Opens against an `alp_adc_stream_t`
   source; runs in the backend's HW DSP slot when available;
   falls back to portable C otherwise.
2. **`<alp/dsp.h>` standalone chain** -- `alp_dsp_chain_t`
   composed of `alp_dsp_stage_t` descriptors (FIR / IIR /
   WINDOW / FFT).  Operates on in-RAM sample buffers.  No
   stream dependency.  Backend selection:
   - **CMSIS-DSP** when `ALP_HAS_CMSIS_DSP=1` (preferred).
   - **Portable C** fallback (always available, slow).

The two surfaces **compose**: an ADC-pipeline stage can wrap an
`alp_dsp_chain_t` so the same chain descriptor that runs against
an in-RAM buffer in offline analysis also runs against a live
ADC stream on-device.

## Consequences

### Positive

- HW DSP offload is exposed natively (the GD32G553 use case).
- Standalone chains exist for offline + headless paths.
- One chain descriptor format across both surfaces — descriptor
  authoring is a one-time learning cost.
- CMSIS-DSP fallback means every backend has working math
  without the HW DSP path.

### Negative

- Two surfaces (`<alp/adc.h>` + `<alp/dsp.h>`) carrying related
  types.  Customer onboarding needs to call out that they
  compose.
- The `alp_dsp_chain_t` standalone surface stays
  `[ABI-EXPERIMENTAL]` for v1.0 (the ADC-pipeline integration
  point may drive descriptor changes); promotion lands when
  the ADC-pipeline + standalone duo is exercised end-to-end on
  real hardware.

### Neutral

- Per-stage configuration uses plain-C structs, not opaque
  handles.  Callers can serialise / deserialise descriptors
  without an SDK callback.  Trade-off: more boilerplate per
  stage; better introspectability.

## Alternatives considered

- **Standalone-only** -- simpler API, but loses the HW DSP path
  on V2N + leaves the GD32G553 FFT/FAC silicon unused.
- **ADC-pipeline-only** -- loses the offline / headless use case
  + ties every customer to having an ADC stream to operate on.
- **Per-primitive surfaces** (`alp_fft.h`, `alp_fir.h`, etc.) --
  flattens the integration story; combinatorial explosion of
  headers + duplicated config logic across each primitive.

## See also

- [`<alp/adc.h>`](../../include/alp/adc.h) — ADC-pipeline types
  (`alp_adc_filter_t`, `alp_adc_spectrum_t`).
- [`<alp/dsp.h>`](../../include/alp/dsp.h) — standalone chain
  surface.
- [`docs/gd32-bridge-protocol.md`](../gd32-bridge-protocol.md)
  §3.z DSP-chain upload opcodes — the wire format that ships
  chain descriptors from the V2N host to the GD32 firmware.
- [memory: wave2-dsp-pipeline-design] — the original maintainer
  redirect captured 2026-05-12.
