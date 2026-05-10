# 0001. Why ALP SDK wraps Zephyr (and why the wrapper stays thin)

Status: Accepted
Date: 2026-05-10

## Context

Zephyr already abstracts vendor-driver diversity below its driver
classes: `i2c_*` works on Alif HAL, Renesas FSP, NXP MCUXpresso.  A
reasonable question is: "if Zephyr already hides Alif vs. Renesas vs.
NXP, why does the ALP SDK add another layer on top?"

The question matters because adding a layer that *only* re-exports
Zephyr's API is dead weight — bug surface without portability gain.
Before committing to wrapping ~12 peripheral classes, the team needed
a clean answer to "what does the wrapper buy us that Zephyr doesn't?"

## Decision

**Yes, ALP SDK wraps Zephyr — but for reasons that are orthogonal to
vendor-diversity-within-Zephyr.**  The wrapper earns its keep on five
distinct fronts:

1. **OS portability.**  The SDK pivots across **three** OS targets:
   Zephyr, Yocto Linux, and bare-metal.  An app written against
   `<alp/i2c.h>` recompiles unchanged across them; an app written
   against `i2c_*` doesn't compile against Linux at all.  This is
   the *central* justification.

2. **Studio codegen target.**  alp-studio's pin allocator emits C
   that calls a fixed API regardless of which OS the active SoM
   uses.  Without the ALP wrapper, codegen would have to fork
   per-OS — multiplying the surface area the studio team
   maintains.

3. **Opaque studio-resolved IDs.**  Zephyr's API takes a
   `const struct device *` (a DT label literal).  The studio
   resolves `bus_id` / `pin_id` / `channel_id` from the e1m-spec
   chain and hands the SDK an integer.  That integer makes apps
   portable across SoMs at the **C-source level** — without the
   wrapper apps would need DT-label substitution at build time.

4. **ABI stability for v1.0.**  Zephyr's APIs can change across
   LTS lines.  The ALP wrapper is the boundary that absorbs Zephyr
   churn.  Apps recompile against any future ALP 1.x.  The ABI
   snapshot at `docs/abi/v0.1-snapshot.json` documents what we'll
   keep.

5. **Co-location of chip drivers + higher libraries.**  LSM6DSO,
   SSD1306, BME280, plus `<alp/audio.h>` / `<alp/camera.h>` /
   `<alp/ble.h>` etc. — these don't belong in Zephyr proper.  They
   belong above it.  The SDK is that layer.

**The wrapper stays thin.**  Re-implementing what Zephyr already does
(DMA schedulers, custom driver classes) adds bug surface without
portability gain.  Each `alp_*_open` is ~40–100 lines: validate
config, look up Zephyr device via DT alias, call Zephyr's open, return
the handle.

## Alternatives

**A. No SDK — apps target Zephyr directly.**  Rejected because:
- Apps don't port to Yocto.  We lose half the OS-pivot story.
- Studio codegen is forced to fork per-OS.
- DT label literals leak into application source — no SoM swap
  without a recompile + DT regen.

**B. SDK as a header-only re-export of Zephyr.**  Rejected because:
- Yocto and bare-metal back-ends don't expose anything to re-export.
  There's nothing to wrap on a Linux host.
- ABI stability is impossible if Zephyr's struct shapes leak
  through.

**C. SDK that *replaces* Zephyr drivers with vendor-direct code.**
Rejected because:
- Zephyr already does the vendor-direct work well; reimplementing it
  is pure cost.
- Vendor partnerships (Alif, Renesas) work through Zephyr upstream.
- Fragmenting the driver world is exactly what the unification SDK
  is supposed to *prevent*.

## Consequences

**Good:**
- Apps written against `<alp/...>` headers are OS-portable and
  SoM-portable at source level.
- alp-studio has a single codegen target.
- Pre-1.0 we can change the wrapper without breaking Zephyr;
  post-1.0 we can absorb Zephyr changes without breaking apps.
- Chip drivers and higher libraries (camera, audio, IoT) have a
  natural home.

**Bad / costs:**
- Apps that only ever target Zephyr-on-AEN see a thin layer with
  small per-call overhead.  We accept this cost in exchange for
  portability.
- Two layers of indirection in the call stack —
  `alp_i2c_write → i2c_write → vendor HAL → silicon`.  Profiling
  the wrapper has shown this adds ≤1 µs on M-class targets.
- The SDK has to track Zephyr's API across LTS bumps and re-test
  every version.  This is mitigated by the ABI snapshot diff in CI.

## See also

- `docs/architecture.md` — "Why this wrapper exists" section.
- `docs/adr/0003-peripheral-coverage.md` — what we *do* and *don't*
  wrap.
