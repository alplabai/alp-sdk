@page docs_adr_index Architecture Decision Records

# Architecture Decision Records

This directory captures the **why** behind significant architectural
choices in the ALP SDK.  Each ADR is a short markdown file describing
one decision: the context that forced it, the alternatives considered,
the choice made, and the consequences (good and bad).

ADRs are append-only — a decision is never edited, only **superseded**
by a later ADR that links back to it.  The index below lists the
current set in chronological order.

## Format

```
# NNNN. <title>

Status: Proposed | Accepted | Superseded by NNNN | Deprecated
Date: YYYY-MM-DD

## Context
What forced the decision.  What problem we were facing.

## Decision
What we chose.

## Alternatives
What else we considered, and why we didn't pick them.

## Consequences
Good: ...
Bad / costs: ...
```

## Index

| ADR  | Title                                                       | Status   |
|------|-------------------------------------------------------------|----------|
| [0001](0001-wrapper-on-top-of-zephyr.md) | Why ALP SDK wraps Zephyr (and why the wrapper stays thin) | Accepted |
| [0002](0002-error-mechanism.md)          | `alp_last_error()` + compile-time SoC capability validation | Accepted |
| [0003](0003-peripheral-coverage.md)      | Wrap 12 peripheral classes at v0.2, not just I2C/SPI/GPIO/UART | Accepted |
| [0004](0004-e1m-portability-bound.md)    | E1M-spec instance counts as the portability bound | Accepted |
| [0005](0005-alp-sdk-vs-alp-studio-boundary.md) | alp-sdk vs alp-studio repo boundary (dual-use acid test) | Accepted |
| [0006](0006-secure-boot-secure-ota.md)   | Secure boot (vendor-native) + secure OTA (MCUboot + Mender) | Accepted, partially superseded (2026-05-11 amendment: Mender replaces RAUC on Linux side; AEN-Zephyr OTA client choice pending) |
| [0007](0007-wave2-dsp-pipeline-design.md) | Wave-2 DSP: pipeline stages, not standalone primitives | Accepted |
| [0008](0008-gpu2d-portable-shim.md)       | GPU2D: portable shim under `<alp/gpu2d.h>` even for single-silicon | Accepted |
| [0009](0009-mender-zephyr-client-deferred.md) | Mender Zephyr client deferred to v1.1 | Accepted |
| [0010](0010-heterogeneous-os-orchestration.md) | Heterogeneous OS orchestration: Zephyr + Yocto as peers, not alternatives | Accepted |
| [0011](0011-intra-family-portability.md) | Intra-family portability is load-bearing; cross-form-factor portability is intentionally not a goal | Accepted |
| [0012](0012-cross-platform-developer-host.md) | alp-sdk developer host is cross-platform; Linux required ONLY for Yocto | Accepted |
| [0013](0013-tfm-boundary-m55-hp-trustzone.md) | TF-M trust boundary on AEN: TrustZone-M split on M55-HP, not a dedicated M55-HE | Accepted |

## When to write an ADR

Write one when the decision:

- Is hard to reverse (changes the public API shape, the build system,
  the OS-pivot story, the way the studio talks to the SDK).
- Has a non-obvious "why" — six months from now the next contributor
  will look at the code and ask "why isn't this done the simpler
  way?"
- Closes off an alternative path that someone could plausibly try
  again ("we considered X, here's why we didn't do it").

Don't write one for ordinary feature work or routine bugfixes —
the changelog is the right place for those.
