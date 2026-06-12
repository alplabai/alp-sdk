# Verification status

> ⚠️ **`v0.6` carries the SDK's first silicon evidence — and only on
> one board.**  The E1M-X **V2N** bench verified the GD32
> supervisor-bridge stack end-to-end (link + soak + A/B OTA + analog
> loopback; see the v0.6.0 section of
> [`docs/test-plan.md`](test-plan.md)).  Every **other** family (AEN,
> i.MX 93, V2M/DEEPX) remains `[UNTESTED]` on real hardware.

This page is the single source of truth for "what's been silicon-
verified in the Alp SDK as of today".  It complements:

- `metadata/chips/<name>.yaml` -- per-chip `verification:` block.
- `metadata/library-profiles/<name>/hw-backends.yaml` -- per-
  library + per-accelerator `verification:` block.
- `@par Verification status` Doxygen tags on every public header.
- `docs/test-plan.md` -- the HiL ledger that flips rows to ✅ as
  evidence lands.

## Where v0.6 actually sits

| Layer | Status | What "passes" today |
|---|---|---|
| Public API (`<alp/*.h>`) | `[ABI-EXPERIMENTAL]` | Doxygen-clean, ABI snapshot tracked, builds on every supported target |
| GD32 supervisor bridge (`firmware/gd32-bridge/` + `chips/gd32g553/`) | `[VERIFIED]` on V2N silicon | fw v0.2.9 / protocol v0.7: functional suite 26/26, 20-row HIL soak 253/253, A/B OTA e2e, Tier-B loopback 5/6 (qenc = carrier wiring, issue #85) |
| CM33↔GD32 SCI7 SPI link (interrupt path) | `[VERIFIED]` on V2N silicon | Sustained bidirectional soak, zero CRC errors (DMA fast path stays default-off — issue #84) |
| Chip drivers (`chips/<name>/`, all others) | `[UNTESTED]` | Build + NULL-arg-guard ZTEST on `native_sim/native/64` |
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
- **The V2N GD32-bridge stack is bench-proven.**  The v0.6 silicon
  campaign verified the supervisor link, OTA, and analog paths on
  the real board -- the per-row evidence lives in
  [`docs/test-plan.md`](test-plan.md).
- **You should NOT ship production firmware against the other
  families (AEN, i.MX 93, V2M) unless you've done your own HiL.**
  Their register addresses, timing values, init sequences have not
  been silicon-validated.

## Verification roadmap

Verification rolls out per-SKU + per-chip as benches come online
(the roadmap is a cherry-pick backlog -- releases tag whatever's
ready).  The V2N bench came online first, so the original
AEN-first order inverted:

1. **v0.6 (done)** -- E1M-V2N101 bring-up.  Verified the GD32
   bridge stack (gd32g553 host driver + firmware: link, soak,
   A/B OTA, DAC/ADC/capture loopback) + the CM33 AMP link +
   the V2N Yocto Linux boot leg: the productized image (eMMC
   HS200, USB-OVC, CA55-cluster watchdog, branded/reproducible
   firmware banners, and the hardened `alp-image-prod` /
   `alp` distro) boots the bench board from eMMC.
2. **next** -- E1M-AEN701 bring-up.  Verifies the AEN-family
   chip drivers (the on-module ones: act8760, da9292,
   clk_5l35023b, etc.) + a representative sample of the §D.AI
   chips (st7789, sh1106, the camera SCCB path).
3. **then** -- E1M-V2M101 (adds DEEPX DX-M1) and E1M-NX9101
   (adds Ethos-U65), bench-availability permitting.
4. **v1.0** -- All four families verified.  `[UNTESTED]` tags
   flip to `[VERIFIED]` per chip as evidence lands.

## Where to find the per-driver status

- Chip driver: `grep "verification:" metadata/chips/<name>.yaml`.
- Library binding: `grep "verification:" metadata/library-profiles/<name>/hw-backends.yaml`.
- Public header: look for `@par Verification status` near the
  top of `include/alp/<surface>.h` or `include/alp/chips/<name>.h`.

## What "passes" for a native_sim-only smoke (the pre-HiL bar)

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

That's HiL territory -- exactly the class the v0.6 V2N campaign
caught for real (an analog subsystem dead until its internal
voltage-reference enable, seven OTA silicon bugs, a capture-wrap
arithmetic slip -- none visible to native_sim).

## Questions

Open an issue with the `verification` label.
