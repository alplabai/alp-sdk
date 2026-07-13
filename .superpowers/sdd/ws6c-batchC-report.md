# WS6-c Phase 1 — Batch C (display/ai/industrial/fs) library manifests

**STATUS: DONE** — 6/6 manifests authored, schema-validated, `validate_metadata.py` clean.

## Files created

- `metadata/libraries/tflite-micro.yaml` (token `tflite_micro`)
- `metadata/libraries/u8g2.yaml` (token `u8g2`, unchanged name)
- `metadata/libraries/gfx-compat.yaml` (token `gfx_compat`)
- `metadata/libraries/madgwick-ahrs.yaml` (token `madgwick_ahrs`)
- `metadata/libraries/mbedtls.yaml` (token `mbedtls`, unchanged name)
- `metadata/libraries/littlefs.yaml` (token `littlefs`, unchanged name)

Each mirrors the `nanopb.yaml`/`etl.yaml` shape: `schema_version`, `name`,
`description`, `tier`, `version`, `license`, `integration.zephyr.{module,
kconfig}`, a top-of-file grounding comment, and a
`# HW-backends: metadata/library-profiles/<token>/hw-backends.yaml ...
folded in WS6-c Phase 3 (needs library-v1 schema extension).` line per the
task's exact wording.

## Validation

```
python3 scripts/validate_metadata.py
```
All 6 new files: `OK`. The run's 4 pre-existing `FAIL` lines
(`libhelix.yaml`, `minimp3.yaml`, `tinygsm.yaml` — license-allowlist
mismatches) belong to a different batch/token set and are untouched by
this change.

## Decisions worth flagging

1. **Tier: B for all 6**, not A. None of the six tokens appears in
   `metadata/registries/tier-a-library-ci.json`'s `hostBuild.libraries` or
   `excludedLibraries`, and per the precedent already set in `etl.yaml`
   (sibling Phase-1 manifest), Tier A requires an entry in that CI-contract
   registry, not just "has a real example." All six already have real,
   native_sim-green teaching examples (audio/ai/camera-vision examples for
   tflite_micro; `examples/display/u8g2-oled-draw`; `examples/display/
   gfx-compat-blit`; `examples/display/drone-hud` +
   `examples/peripheral-io/drone-autopilot` for madgwick_ahrs; several
   `examples/connectivity/*` + `examples/multicore/*` for mbedtls;
   `examples/power-timing/littlefs-keyvalue` for littlefs) — promotion to
   Tier A is a named follow-up (add the CI-contract entries), out of scope
   for this additive Phase-1 pass.

2. **Comment-string tuple elements not copied into `kconfig:`.** For
   `tflite_micro` and `gfx_compat`, `_LIBRARY_KCONFIG`'s tuple has a second
   element that is a Python comment string (e.g. `"# tflite_micro: include
   path + tflm_config.h via v0.4 loader hook"`), not a real `CONFIG_...=...`
   assignment. The schema's `kconfig` items must match
   `^CONFIG_[A-Z0-9_]+=[A-Za-z0-9_"-]+$`, so copying the comment literally
   would fail validation. Only the real `CONFIG_ALP_TFLM_REF_KERNELS=y` /
   `CONFIG_ALP_GFX_COMPAT_SW=y` lines went into `kconfig:`; the comment text
   is preserved as prose in the top-of-file grounding comment instead.

3. **CONCERN (tflite-micro): base enable symbol not emitted today.**
   `scripts/alp_project_emit/__init__.py`'s `_LIBRARY_KCONFIG["tflite_micro"]`
   never sets the real upstream enable symbol `CONFIG_TENSORFLOW_LITE_MICRO=y`
   (confirmed present at `$ZEPHYR_BASE/modules/tflite-micro/Kconfig`) — only
   the ALP-side `CONFIG_ALP_TFLM_REF_KERNELS=y` fallback marker. This
   manifest transcribes exactly what the emitter sets today per the
   Phase-1 "copy exact kconfig, never invent" rule; wiring the real enable
   symbol is a Phase 3 emitter fix, not a Phase 1 manifest one. Flagged in
   `tflite-micro.yaml`'s header comment.

4. **CONCERN (tflite-micro): version SHA is best-effort.** No project
   stanza for `tflite-micro` exists in this repo's own `west.yml` text — it
   rides in via Zephyr's own v4.4.0 manifest through the
   `modules.name-allowlist` passthrough. This workspace's local
   `zephyrproject` checkout doesn't have that module's source tree fetched
   (only the 2-file Kconfig/CMakeLists glue). The `version:` field
   (`fcc760af130f3a595b5802cdebcc77461e54f382`) is grounded from a separate
   local reference clone (`~/tflite-micro`, tracking
   `zephyrproject-rtos/tflite-micro`'s `zephyr-v4.4.0` branch) — flagged as
   best-effort pending confirmation against the live upstream Zephyr
   v4.4.0 manifest text.

5. **CONCERN (madgwick-ahrs): upstream pin floats on `main`.** `west.yml`
   pins `madgwick_ahrs` (repo-path `Fusion`, xio-technologies) to the
   branch `main` with an explicit `# TBD: pin SHA after maintainer audit`
   comment — this predates this change and is a real ADR 0017 ("pin, don't
   float") gap upstream of this manifest. `version:` is written as an
   honest placeholder string
   (`"unpinned (west.yml tracks xio-technologies/Fusion@main; TBD pin SHA
   after maintainer audit)"`) rather than fabricating a commit SHA. License
   (`MIT`) is cited from general knowledge of the upstream project, not
   re-verified from a local LICENSE file (none checked out in this
   workspace) — lowest-confidence fact in this batch.

6. **Main concern to carry into Phase 3**: all six `hw-backends.yaml`
   files (tflite_micro, u8g2, gfx_compat, madgwick_ahrs, mbedtls, littlefs)
   carry real per-accelerator priority models (`accelerators: [{class,
   priority: [{requires_cap/silicon, backend, kconfig}]}]`,
   `sw_fallback`, `verification`) that the current `library-v1.schema.json`
   has no room for. Every manifest in this batch references its
   `hw-backends.yaml` file by path with the exact prescribed comment
   wording and does NOT fold the accelerator data in — that fold needs a
   schema extension (new `integration.zephyr.hw_backends` or similar) as a
   named Phase 3 task, not something to improvise per-manifest now.

## Gate run

```
python3 scripts/validate_metadata.py
```
Result: `OK` for all 6 new manifests (plus all pre-existing manifests
except the 3 pre-existing unrelated license failures noted above).
