# 0004. E1M-spec instance counts as the portability bound

Status: Accepted
Date: 2026-05-10

## Context

The E1M open standard (`alpCaner/e1m-spec`) fixes which peripheral
instances every conformant SoM SHALL route, and pins each instance's
primary function to specific physical pads.  For example:

- `PWM0..PWM7` are reserved at fixed pads (A6, A5, A4, A3, …).
- `ENC0..ENC3` are reserved at fixed pads (A7-A8, …).
- `CAN0` is reserved at one specific pad pair.

But individual SoCs typically expose **more** instances than E1M
reserves.  Renesas RZ/V2N has 6 CAN channels; only one is routed
through to E1M's `CAN0` pads.  The other five are vendor-specific.

Two competing pressures:

1. **Portability.**  Apps that target only the E1M reservations
   should work unchanged when the SoM swaps from AEN (Alif) to V2N
   (Renesas) to i.MX 93 (NXP).  This is the SDK's central promise.
2. **Useful coverage.**  V2N apps might genuinely want to use the
   five vendor-specific CAN channels — the wrapper shouldn't reject
   those just to enforce portability.

How does the wrapper navigate?

## Decision

**Three concentric capability bounds**, with explicit
responsibilities at each layer:

```
              tightest                                          loosest
              ────────                                          ───────
  E1M reservation  <  Studio block declaration  <  SoC count  <  driver array
       (8 PWMs)       (block uses PWM0..PWM3)    (12 timers)    (8 entries)
```

1. **E1M reservation** — `ALP_E1M_<CLASS>_COUNT` macros in
   `<alp/e1m_pinout.h>`.  This is the portability bound.  Apps that
   use `ALP_E1M_<CLASS><N>` for `N < ALP_E1M_<CLASS>_COUNT` are
   guaranteed cross-SoM compatibility.  alp-studio's pin allocator
   enforces this when generating code for portable blocks.

2. **Studio block declaration** — each block manifests its required
   instances.  Studio rejects blocks whose declarations exceed E1M.

3. **SoC count** — `ALP_SOC_<CLASS>_COUNT` from
   `include/alp/soc_caps.h` (generated from
   `metadata/socs/**.json`).  This is what the SDK's runtime
   `*_open` validates against.  Vendor-specific extensions live in
   the gap between `ALP_E1M_*_COUNT` and `ALP_SOC_*_COUNT`.

4. **Driver array** — the upper bound declared by the SDK's
   per-class backend (e.g. `peripheral_can.c` declares 6 entries).
   Sized to the most expansive supported SoC; purely defensive.

## Alternatives

**A. Cap every wrapper at the E1M reservation.**  Rejected because
V2N apps couldn't access the SoC's vendor-specific CAN channels
without a separate vendor wrapper — exactly the kind of fragmentation
the SDK exists to prevent.

**B. Don't expose `ALP_E1M_*_COUNT` at all; rely on the studio.**
Rejected because hand-written firmware needs portability hints too,
and the constants are useful documentation regardless.

**C. Make `ALP_E1M_<CLASS>_COUNT` enforced at compile time via
linker errors.**  Rejected — the wrapper's job is to *allow* both
portable and non-portable use; the studio's job is to enforce
portability per-block.  Tying the SDK's hands removes a useful
escape hatch.

## Consequences

**Good:**
- Apps choose: portable (ALP_E1M_*) or vendor-extended (raw integers
  up to ALP_SOC_*).  Neither path is blocked.
- The portability discipline lives in alp-studio + manifests; the
  SDK is responsibility-light, just provides the bounds.
- Adding a new peripheral class to E1M v1.x is a one-line
  `#define ALP_E1M_<CLASS>_COUNT N` addition plus an ADR pointer to
  the spec change.

**Bad / costs:**
- Three sets of count constants to keep aligned across e1m-spec
  releases.  Mitigated because `gen_soc_caps.py` reads metadata
  files that already encode SoC counts; only `e1m_pinout.h` is
  hand-curated.
- Apps that mix portable and vendor-extension calls (e.g.
  `ALP_E1M_CAN0` + `bus_id = 1`) silently lose portability.  Code
  review and the studio's per-block check are the safety net.

## See also

- `include/alp/e1m_pinout.h` — the `ALP_E1M_*_COUNT` definitions.
- `docs/architecture.md` — "E1M as the portability bound" section.
- `docs/e1m-pinout.md` — how the studio threads pin assignments
  through the e1m-spec → SoM manifest → SDK chain.
- `alpCaner/e1m-spec` — the canonical pinout standard (currently
  v1.1; SDK pinned to v1.0).
