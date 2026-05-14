# Verification status

> ⚠️ **`v0.5` is `[UNTESTED]` end-to-end.  Nothing has touched real
> silicon yet.**

This page is the single source of truth for "what's been silicon-
verified in the ALP SDK as of today".  It complements:

- `metadata/chips/<name>.yaml` -- per-chip `verification:` block.
- `metadata/library-profiles/<name>/hw-backends.yaml` -- per-
  library + per-accelerator `verification:` block.
- `@par Verification status` Doxygen tags on every public header.
- `docs/test-plan.md` -- the HiL ledger that flips rows to ✅ as
  evidence lands.

## Where v0.5 actually sits

| Layer | Status | What "passes" today |
|---|---|---|
| Public API (`<alp/*.h>`) | `[ABI-EXPERIMENTAL]` | Doxygen-clean, ABI snapshot tracked, builds on every supported target |
| Chip drivers (`chips/<name>/`) | `[UNTESTED]` | Build + NULL-arg-guard ZTEST on `native_sim/native/64` |
| Library bindings (`metadata/library-profiles/<name>/`) | `[UNTESTED]` | Schema validated; emission unit-tested in `tests/scripts/test_alp_project.py`; build-time linkable for Zephyr-native libs |
| Per-SoM `capabilities:` blocks | `[PARTIAL]` | Populated from datasheet readings; some fields marked `# TBD` for items pending datasheet verification |
| Per-NPU TFLM driver gates (`CONFIG_ALP_TFLM_ETHOS_U85/U65/U55`) | `[UNTESTED]` | Kconfig-reachable; no Vela-compiled model has actually been dispatched yet |
| Examples + reference apps | `[UNTESTED]` | Build clean on native_sim; HiL flash-and-run still TBD |
| CI: pr-twister + pr-static-analysis + pr-doxygen | ✅ green on `main` | Build correctness, style, doc completeness |
| CI: pr-twister with `--platform alif_*` | ✅ build-only | Cross-compiles to AEN target; doesn't flash silicon |
| HiL runners | ❌ not online | `nightly-aen-hil.yml` is a skeleton waiting for a self-hosted runner |

## What this means for customers

- **You can design against the API.**  The public headers are
  stable enough -- the `[ABI-EXPERIMENTAL]` tag tracks intent
  to keep them stable from v0.5 onward; the ABI-snapshot CI
  catches breaking changes.
- **You can pick chips + libraries from the registry.**  The
  binding matrix (`metadata/library-profiles/<name>/hw-backends.yaml`
  + `metadata/e1m_modules/<sku>.yaml`'s `capabilities:` block) is
  the contract we'll honour once HiL runs.
- **You can write portable apps.**  Examples build on `native_sim`
  today; they'll port to real hardware as HiL evidence accumulates.
- **You should NOT ship production firmware against v0.5 unless
  you've done your own HiL.**  Register addresses, timing values,
  init sequences -- none have been silicon-validated.

## Verification roadmap

Verification rolls out per-SKU + per-chip from v0.6 onward.  The
order is driven by the E1M-AEN701 EVK availability + the test-
plan ledger in `docs/test-plan.md`:

1. **v0.6** -- E1M-AEN701 bring-up.  Verifies the AEN-family
   chip drivers (the on-module ones: act8760, da9292,
   clk_5l35023b, etc.) + a representative sample of the §D.AI
   chips (st7789, sh1106, the camera SCCB path).
2. **v0.7** -- E1M-V2N101 bring-up.  Verifies the V2N family +
   the GD32 bridge (gd32g553, gd32_swd, cau, tmu_*).
3. **v0.8** -- E1M-V2M101 bring-up.  Adds DEEPX DX-M1.
4. **v0.9** -- E1M-NX9101 bring-up.  Adds Ethos-U65.
5. **v1.0** -- All four families verified.  `[UNTESTED]` tags
   flip to `[VERIFIED]` per chip as evidence lands.

## Where to find the per-driver status

- Chip driver: `grep "verification:" metadata/chips/<name>.yaml`.
- Library binding: `grep "verification:" metadata/library-profiles/<name>/hw-backends.yaml`.
- Public header: look for `@par Verification status` near the
  top of `include/alp/<surface>.h` or `include/alp/chips/<name>.h`.

## What "passes" for a v0.5-style smoke

The ZTEST suite (`tests/zephyr/`) catches:

- Build regressions (the file compiles + links).
- NULL-arg defense (every public function returns
  `ALP_ERR_INVAL` on a NULL handle).
- Loader emission (the §D.lib `_emit_library_hw_backends()`
  picks the right per-SKU `CONFIG_*=y` set -- see
  `tests/scripts/test_alp_project.py`'s `TestHwBackendsLoader`).
- Kconfig reachability (the per-library SW-fallback knobs all
  resolve at parse time -- see
  `tests/zephyr/library_knobs/`).

The ZTEST suite does NOT catch:

- A wrong I²C register address that the driver writes.
- A timing bug that needs a scope to see.
- An Ethos-U / DRP-AI / DEEPX dispatch that returns nonsense.
- A capacitor missing on the schematic.

That's all v0.6+ HiL territory.

## Questions

`#untested-v05` in the community forum; or open an issue with
the `verification` label.
