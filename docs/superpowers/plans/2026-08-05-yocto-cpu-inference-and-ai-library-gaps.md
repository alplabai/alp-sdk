# Yocto CPU Inference + AI Library Gaps Implementation Plan

> **STATUS — LANDED.** All seven tasks shipped: `metadata/libraries/madgwick-ahrs.yaml` is SHA-pinned, `onnx` is in `scripts/alp_model/manifest.py`'s `VALID_BLOB_FORMATS`, `metadata/libraries/onnxruntime.yaml` declares 1.28.0, and `src/yocto/inference_ort.cpp` + `src/yocto/CMakeLists.txt` carry the ORT CPU backend. Kept for implementation-history context.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. (Superseded by the status banner above -- do not execute without reading it first.)

**Goal:** Give the Cortex-A55 SoMs (E1M-V2N101/102, E1M-V2M101/102, E1M-NX9101) a real CPU inference path under Yocto by adding ONNX Runtime as the `ALP_INFERENCE_BACKEND_CPU` implementation, and close the two metadata-hygiene defects found alongside it.

**Architecture:** `src/yocto/inference_yocto.c` already dispatches to per-backend hook sets (`alp_inference_<backend>_*`), each compiled in behind an `ALP_SDK_USE_<BACKEND>` CMake option, with `resolve_auto()` choosing priority. A new `src/yocto/inference_ort.cpp` supplies that same seven-hook contract for ONNX Runtime, wired as the CPU backend — the slot whose header comment currently reads "Wiring deferred to v0.4". Nothing about the existing DEEPX/DRP-AI backends changes; CPU stays lowest priority in `resolve_auto()` so an NPU-bearing SoM still selects its NPU under `AUTO`.

**Tech Stack:** ONNX Runtime (MIT), Yocto/BitBake (`meta-alp-sdk`, LAYERSERIES_COMPAT kirkstone scarthgap), C++17, CMake, Python 3 for the model-adapter layer, pytest.

## Global Constraints

- Library license must be one of the `metadata/schemas/library-v1.schema.json` enum: `Apache-2.0`, `MIT`, `BSD-2-Clause`, `BSD-3-Clause`, `Zlib`, `MIT-0`, `BSL-1.0`, `CC0-1.0`. ONNX Runtime is `MIT` — verified at `repos/microsoft/onnxruntime`.
- ADR 0018 tiers are **A** and **B** only. There is no tier C.
- ADR 0018 non-goals forbid in-tree vendored copies of library sources. No `vendors/onnxruntime/`.
- Every third-party pin is a tag or a full SHA. Never a floating branch.
- alp-sdk C house style: tabs, clang-format 22.x, `clang-format · diff-only` must print nothing.
- Public API additions require an ABI-snapshot regen (`scripts/abi_snapshot.py`) — see Task 2's commit step.
- `docs/portability-matrix.md` is generated between `<!-- BEGIN/END GENERATED: gen_portability_matrix_libraries -->` markers. Regenerate with `python3 scripts/gen_portability_matrix.py`; never hand-edit.
- Branch off `dev`, PR to `dev`. Full local gate set (`bash scripts/test-all.sh --target dev`) green before `gh pr create`.

---

## Scope Split — Read Before Starting

The AI-surface gaps span four independent subsystems. **This plan covers only the first two.** The other two are deliberately separate plans because they are bench-gated and their implementation steps cannot be written honestly without hardware access and vendor BSP grounding I do not have:

| Subsystem | Covered here? | Why |
|---|---|---|
| Metadata hygiene (`madgwick-ahrs` unpinned) | **Yes** — Task 1 | Fully specifiable now |
| Yocto CPU inference via ONNX Runtime | **Yes** — Tasks 2–7 | Fully specifiable now |
| DEEPX on-silicon verification | **No** — separate plan | Code is real but has never run on hardware. Needs a held labgrid reservation and `alp-bench-runner`; steps depend on what the first run shows. Bench-before-merge applies. |
| Ethos-U65 attach on E1M-NX9101 | **No** — separate plan | `src/backends/inference/ethos_u_n93.cpp` is a deliberate stub pending NXP BSP maturity. The `vela` adapter is already real, so the missing piece is the BSP attach. |

Do not fold those two into this plan. Each produces working, testable software on its own.

## Status Update — 2026-08-05, after this plan was written

Three things changed. Read this before executing anything below.

**Task 1 is DONE, and it was three defects, not one.** Three manifests floated,
and two were broken outright rather than merely unpinned: `minimp3` pinned
`revision: main`, a branch that does not exist upstream (HTTP 422; default is
`master`), and BearSSL's remote `https://github.com/bearsslmirror/BearSSL` returns
"Repository not found". `west update --group-filter +extras-tier1` could not fetch
either. Final pins: `madgwick_ahrs` → tag `v1.3.2` = `015d68494274b479b5996bff2530ecbcfdc266f2`;
`minimp3` → `ea99364f61c14656440e8d77e9c233ccf3124633`; BearSSL → remote repointed
to upstream `https://bearssl.org/git/BearSSL`, pinned `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`.
Root cause of the survival: `.github/workflows/nightly-extras-tier1-pins.yml` had
a warn-only exemption naming exactly those three; all eleven Tier-1 libraries now
FAIL on a failed fetch. Shipped as PR #1246.

**DRP-AI is no longer a gap.** PR #1238 (`cbea0e29`) landed the real
`MeraDrpRuntimeWrapper` implementation in `src/yocto/inference_drpai.cpp` behind
two independent default-OFF switches (`PACKAGECONFIG[drpai]` compiles the backend;
`ALP_ENABLE_DRPAI = "1"` installs the DT override claiming the `0xd0000000` /
`0x20000000` carve-out), plus `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm/`.
The deferred bench plan is therefore DEEPX-only now.

**Task 4's fork has been resolved, and it invalidates this plan's premise.**
`bitbake` is not available on this host, so the question was answered from the
layer sources directly:

- `openembedded-core` and `meta-openembedded` — the only two layers
  `meta-alp-sdk/conf/layer.conf` declares in `LAYERDEPENDS_alp-sdk` — ship **no**
  ONNX Runtime recipe at all. So Task 4 Step 1's "stop and report" branch fires:
  there is nothing to `.bbappend`.
- Both A55 vendor stacks ship one, at incompatible versions:
  `renesas-rz/meta-renesas-ai` → `recipes-mathematics/onnxruntime/onnxruntime_1.8.0.bb`
  (RZ/V2N, V2M) and `nxp-imx/meta-imx` → `meta-imx-ml/recipes-libraries/onnxruntime/`
  at **1.24.3** (i.MX 93 / NX9101).

Tasks 3–7 assume ONE ONNX Runtime version serving every A55 SoM — that is what
makes a single `metadata/libraries/onnxruntime.yaml` with one `version:` field
honest. A 1.8.0-vs-1.24.3 split across families breaks it. **Do not execute
Tasks 3–7 until this is decided.** The options, none free:

1. Depend on the per-family vendor ML layer. Cheapest to build, but the manifest's
   single `version:` field then lies on one family or the other, and it drags in
   vendor layers with their own licensing.
2. Write `meta-alp-sdk`'s own `onnxruntime_<version>.bb`. One version everywhere,
   honest manifest — but a from-scratch recipe for a project this size is its own
   plan, exactly as Task 4 Step 1 warns.
3. Scope ORT to one family first (NX9101, where 1.24.3 is current) and declare the
   manifest family-scoped via `requires`, deferring V2N.

A side-find for the deferred Ethos-U65 plan: `meta-imx-ml/recipes-libraries/` also
ships `ethos-u-vela` and `ethos-u-driver-stack` — the pieces that attach needs.

---

## File Structure

| File | Responsibility |
|---|---|
| `metadata/libraries/madgwick-ahrs.yaml` | **Modify** — replace the floating `main` pin with a SHA |
| `metadata/libraries/onnxruntime.yaml` | **Create** — ADR 0018 manifest, tier B, Yocto-only (`integration.yocto`), no Zephyr section |
| `scripts/alp_model/manifest.py` | **Modify** — add `onnx` to the `blob_format` set |
| `scripts/alp_model/adapters/__init__.py` | **Modify** — same `blob_format` docstring, keep the two in sync |
| `meta-alp-sdk/recipes-devtools/onnxruntime/onnxruntime_%.bbappend` | **Create** — pin + configure the upstream recipe for A55 |
| `src/yocto/inference_ort.cpp` | **Create** — the seven `alp_inference_ort_*` hooks |
| `src/yocto/CMakeLists.txt` | **Modify** — `ALP_SDK_USE_ORT_CPU` option + conditional source |
| `src/yocto/inference_yocto.c` | **Modify** — declare hooks, add switch arm, extend `resolve_auto()` |
| `tests/scripts/test_alp_model_adapters.py` | **Modify** — assert `onnx` is an accepted `blob_format` |
| `docs/recommended-libraries.md`, `metadata/libraries/README.md`, `CHANGELOG.md` | **Modify** — curated-set surfaces |

---

### Task 1: Pin `madgwick-ahrs` to a real SHA

Standalone and independently shippable — do this first, it needs none of the ONNX work.

**Files:**
- Modify: `metadata/libraries/madgwick-ahrs.yaml`
- Modify: `west.yml` (the `madgwick_ahrs` project's `revision:`)
- Test: `tests/scripts/test_library_layer.py`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Resolve the real upstream SHA**

```bash
gh api repos/xioTechnologies/Fusion/commits/main --jq '.sha, .commit.committer.date'
```

Record both. The repo currently has no semver tags — confirm with:

```bash
gh api "repos/xioTechnologies/Fusion/tags?per_page=5" --jq '.[].name'
```

If that returns tag names, **prefer the newest tag over the SHA** and use it in both files below.

- [ ] **Step 2: Write the failing test**

Add to `tests/scripts/test_library_layer.py`:

```python
def test_no_library_manifest_tracks_a_floating_branch():
    """A floating `main`/`master` pin is a supply-chain hole: the build is not
    reproducible and an upstream force-push silently changes what we ship."""
    floating = {"main", "master", "HEAD", "trunk", ""}
    offenders = []
    for path in sorted((REPO / "metadata" / "libraries").glob("*.yaml")):
        manifest = yaml.safe_load(path.read_text(encoding="utf-8"))
        if str(manifest.get("version", "")).strip() in floating:
            offenders.append(path.name)
    assert offenders == [], f"floating version pins: {offenders}"
```

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_library_layer.py::test_no_library_manifest_tracks_a_floating_branch -v`
Expected: FAIL, listing `madgwick-ahrs.yaml`.

- [ ] **Step 4: Apply the pin**

In `metadata/libraries/madgwick-ahrs.yaml`, replace the `version:` value with the SHA (or tag) from Step 1 and delete the "TBD pin SHA" note. In `west.yml`, set the `madgwick_ahrs` project's `revision:` to the same value and remove its `# TBD` comment.

Both files must carry the identical value — that is the invariant this task exists to establish.

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest tests/scripts/test_library_layer.py -q`
Expected: PASS.

- [ ] **Step 6: Regenerate the lock and the matrix**

```bash
python3 scripts/west_commands/alp_lock.py --workspace .
python3 scripts/gen_portability_matrix.py
python3 scripts/gen_portability_matrix.py --check
```

Expected: `--check` prints `OK docs/portability-matrix.md (in sync)`.

- [ ] **Step 7: Commit**

```bash
git add metadata/libraries/madgwick-ahrs.yaml west.yml alp.lock docs/portability-matrix.md tests/scripts/test_library_layer.py
git commit -m "fix(libs): pin madgwick-ahrs instead of tracking a floating branch"
```

---

### Task 2: Add `onnx` to the model-manifest `blob_format` set

**Files:**
- Modify: `scripts/alp_model/manifest.py:38`
- Modify: `scripts/alp_model/adapters/__init__.py:10-20`
- Test: `tests/scripts/test_alp_model_adapters.py`

**Interfaces:**
- Consumes: nothing.
- Produces: the string literal `"onnx"` as a valid `blob_format`. Task 5 relies on it to tag ORT-consumable blobs.

- [ ] **Step 1: Write the failing test**

Add to `tests/scripts/test_alp_model_adapters.py`:

```python
def test_onnx_is_an_accepted_blob_format():
    """The ORT CPU backend consumes raw .onnx, which is neither a vela_tflite
    nor a dxnn nor a drpai_dir blob."""
    from alp_model import manifest
    assert "onnx" in manifest.VALID_BLOB_FORMATS
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_alp_model_adapters.py::test_onnx_is_an_accepted_blob_format -v`
Expected: FAIL — either `AttributeError: VALID_BLOB_FORMATS` (the set is currently only a comment) or `AssertionError`.

- [ ] **Step 3: Promote the comment to a real constant and add `onnx`**

`scripts/alp_model/manifest.py` currently documents the set only in a trailing comment on the `blob_format` field. Replace that with an enforced constant:

```python
# The blob formats the SDK can describe. Kept as a real constant rather than a
# comment so a new backend cannot silently invent a format string.
VALID_BLOB_FORMATS = frozenset({"vela_tflite", "tflite", "drpai_dir", "dxnn", "onnx"})
```

Mirror the same list in the `Blob` docstring in `scripts/alp_model/adapters/__init__.py` so the two do not drift.

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/scripts/test_alp_model_adapters.py -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/manifest.py scripts/alp_model/adapters/__init__.py tests/scripts/test_alp_model_adapters.py
git commit -m "feat(model): accept onnx as a blob_format"
```

---

### Task 3: Add the ONNX Runtime library manifest

**Files:**
- Create: `metadata/libraries/onnxruntime.yaml`
- Test: `tests/scripts/test_library_layer.py`

**Interfaces:**
- Consumes: nothing.
- Produces: the library name `onnxruntime`, selectable via `board.yaml` `libraries: [onnxruntime]`. Task 4's recipe supplies what `image_install` names.

- [ ] **Step 1: Resolve the upstream pin**

```bash
gh api "repos/microsoft/onnxruntime/tags?per_page=10" --jq '.[].name'
gh api repos/microsoft/onnxruntime/commits/<chosen-tag> --jq '.sha, .commit.committer.date'
```

Pick the newest stable `v*` tag. Record tag and SHA; both go in the manifest header as grounding.

- [ ] **Step 2: Write the failing test**

Add to `tests/scripts/test_library_layer.py`:

```python
def test_onnxruntime_is_yocto_only_with_no_zephyr_section():
    """ORT targets the A55 Linux side only. A zephyr section would imply an
    M-class build we do not ship."""
    manifest = yaml.safe_load(
        (REPO / "metadata" / "libraries" / "onnxruntime.yaml").read_text(encoding="utf-8")
    )
    assert manifest["tier"] == "B"
    assert manifest["license"] == "MIT"
    assert "yocto" in manifest["integration"]
    assert "zephyr" not in manifest["integration"]
    assert manifest["requires"]["core_class"] == "a"
```

- [ ] **Step 3: Run test to verify it fails**

Run: `python3 -m pytest tests/scripts/test_library_layer.py::test_onnxruntime_is_yocto_only_with_no_zephyr_section -v`
Expected: FAIL with `FileNotFoundError`.

- [ ] **Step 4: Write the manifest**

Create `metadata/libraries/onnxruntime.yaml`, following the house shape of `metadata/libraries/ros2.yaml` (the existing Yocto-only precedent):

```yaml
# ONNX Runtime -- cross-platform inference runtime, the CPU backend for the
# Cortex-A55 SoMs under Yocto (ADR 0018 library manifest).
#
# Fills the ALP_INFERENCE_BACKEND_CPU slot that src/yocto/inference_yocto.c
# documents as "Wiring deferred to v0.4". Applies to every A55-bearing SKU:
# E1M-V2N101/102, E1M-V2M101/102, E1M-NX9101. It does NOT replace an NPU --
# resolve_auto() still prefers DEEPX DX-M1 and DRP-AI where present; ORT is
# the portable floor and the path for models no NPU compiler accepts.
#
# Grounding (github.com/microsoft/onnxruntime):
#   * licence MIT (repo /license endpoint)
#   * pin <TAG> = <SHA>, resolved in Task 3 Step 1
#   * NOT a Zephyr module: there is no zephyr/module.yml upstream and the
#     runtime targets a full Linux userspace, so this manifest carries no
#     zephyr section at all.
schema_version: 1
name: onnxruntime
description: "ONNX Runtime -- CPU inference runtime for the Cortex-A55 Linux side."
tier: B
version: "<TAG>"
license: MIT

requires:
  # A-class only: needs a full Linux userspace, a filesystem and threads.
  core_class: a
  os:
    - yocto

integration:
  yocto:
    image_install:
      - onnxruntime
```

Replace `<TAG>` and `<SHA>` with the Step 1 values before committing.

- [ ] **Step 5: Run test to verify it passes**

Run: `python3 -m pytest tests/scripts/test_library_layer.py -q && python3 scripts/validate_metadata.py`
Expected: both PASS, `validate_metadata.py` reporting 0 failures.

- [ ] **Step 6: Regenerate the matrix and commit**

```bash
python3 scripts/gen_portability_matrix.py
python3 scripts/gen_portability_matrix.py --check
git add metadata/libraries/onnxruntime.yaml docs/portability-matrix.md tests/scripts/test_library_layer.py
git commit -m "feat(libs): curate onnxruntime as a Tier B Yocto library"
```

---

### Task 4: Add the ONNX Runtime Yocto recipe append

**Files:**
- Create: `meta-alp-sdk/recipes-devtools/onnxruntime/onnxruntime_%.bbappend`
- Test: manual bitbake parse (below)

**Interfaces:**
- Consumes: the library name from Task 3's `image_install`.
- Produces: an `onnxruntime` package providing `libonnxruntime.so` and the `onnxruntime_c_api.h` header that Task 5 compiles against.

- [ ] **Step 1: Determine whether an upstream recipe exists**

`meta-alp-sdk/conf/layer.conf` declares `LAYERDEPENDS_alp-sdk = "core openembedded-layer"`. Check whether the configured layer set already provides ONNX Runtime:

```bash
bitbake-layers show-recipes onnxruntime
```

- If a recipe exists, this task is a `.bbappend` that pins and configures it.
- If none exists, this task instead creates a full `onnxruntime_<version>.bb`, and the plan's file list changes accordingly. **Stop and report** before writing a full recipe from scratch — a new recipe for a project this size is its own plan, not a step in this one.

- [ ] **Step 2: Write the append**

Assuming Step 1 found an upstream recipe, create the `.bbappend` pinning the Task 3 version and disabling the CUDA/TensorRT execution providers, which are meaningless on A55 and pull enormous dependencies:

```
# SPDX-License-Identifier: Apache-2.0
#
# ONNX Runtime for the Cortex-A55 SoMs. Pinned to the same version
# metadata/libraries/onnxruntime.yaml declares -- the manifest is the
# single source of truth for the version; this append must not drift.

PACKAGECONFIG:remove = "cuda tensorrt"
```

- [ ] **Step 3: Verify the recipe parses**

```bash
bitbake -p
bitbake-layers show-recipes onnxruntime
```

Expected: parse completes with no error and the recipe resolves at the pinned version.

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-devtools/onnxruntime/
git commit -m "feat(yocto): pin and configure onnxruntime for the A55 SoMs"
```

---

### Task 5: Implement the ORT backend hooks

**Files:**
- Create: `src/yocto/inference_ort.cpp`
- Modify: `src/yocto/CMakeLists.txt`

**Interfaces:**
- Consumes: `struct alp_inference`, `alp_inference_config_t`, `alp_inference_tensor_t` from `include/alp/inference.h`; the ORT C API from `onnxruntime_c_api.h`.
- Produces: exactly these seven symbols, matching the shape `src/yocto/inference_yocto.c` already declares for DEEPX and DRP-AI:
  - `alp_status_t alp_inference_ort_open(struct alp_inference *h, const alp_inference_config_t *cfg)`
  - `size_t alp_inference_ort_num_inputs(struct alp_inference *h)`
  - `size_t alp_inference_ort_num_outputs(struct alp_inference *h)`
  - `alp_status_t alp_inference_ort_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)`
  - `alp_status_t alp_inference_ort_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)`
  - `alp_status_t alp_inference_ort_invoke(struct alp_inference *h)`
  - `void alp_inference_ort_close(struct alp_inference *h)`

- [ ] **Step 1: Read the reference implementation first**

Read `src/yocto/inference_deepx.cpp` end to end before writing anything. It is the closest sibling — same hook contract, same C-linkage-from-C++ pattern, same last-error discipline. Match its structure, its `extern "C"` placement, and its error mapping rather than inventing a new shape.

- [ ] **Step 2: Write the failing build**

Create `src/yocto/inference_ort.cpp` with the seven hooks. Each returns `ALP_ERR_NOSUPPORT` for now except `open`, which validates arguments:

```cpp
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ONNX Runtime CPU backend for the Cortex-A55 Linux side.
 *
 * Fills the ALP_INFERENCE_BACKEND_CPU slot. Lowest priority in
 * resolve_auto(): an NPU-bearing SoM still selects its NPU under AUTO,
 * and this backend is the portable floor.
 */
extern "C" {

alp_status_t alp_inference_ort_open(struct alp_inference *h, const alp_inference_config_t *cfg)
{
	if (h == NULL || cfg == NULL || cfg->model_data == NULL || cfg->model_size == 0) {
		return ALP_ERR_INVAL;
	}
	return ALP_ERR_NOSUPPORT;
}

} /* extern "C" */
```

Add the remaining six hooks in the same shape.

- [ ] **Step 3: Wire the CMake option**

In `src/yocto/CMakeLists.txt`, mirror how `ALP_SDK_USE_DEEPX_DXM1` is declared:

```cmake
option(ALP_SDK_USE_ORT_CPU "Build the ONNX Runtime CPU inference backend" OFF)
if(ALP_SDK_USE_ORT_CPU)
	target_sources(alp PRIVATE inference_ort.cpp)
	target_compile_definitions(alp PRIVATE ALP_SDK_USE_ORT_CPU)
	target_link_libraries(alp PRIVATE onnxruntime)
endif()
```

Default `OFF` matches `ALP_SDK_USE_DRPAI_V2N`'s posture: a backend that has not run on silicon does not default on.

- [ ] **Step 4: Verify it compiles**

```bash
cmake -B build/yocto-ort -S . -DALP_OS=yocto -DALP_SDK_USE_ORT_CPU=ON
cmake --build build/yocto-ort --parallel
```

Expected: clean build. If `onnxruntime_c_api.h` is not found, Task 4's recipe did not stage its headers — fix that before continuing.

- [ ] **Step 5: Replace the stubs with the real ORT calls**

Implement `open` to create an `OrtEnv` and `OrtSession` from `cfg->model_data`/`cfg->model_size` via `CreateSessionFromArray`, cache input/output counts and tensor metadata on the handle, and map every ORT failure onto an `alp_status_t`. Implement `invoke` via `OrtApi::Run`. Implement `close` to release the session then the env, in that order, and to tolerate a NULL handle.

Follow `inference_deepx.cpp`'s error-mapping table exactly rather than inventing new mappings.

- [ ] **Step 6: Verify it still compiles and format it**

```bash
cmake --build build/yocto-ort --parallel
git diff -U0 origin/dev -- "*.c" "*.h" "*.cpp" | clang-format-diff.py -p1
```

Expected: build clean, clang-format output empty.

- [ ] **Step 7: Commit**

```bash
git add src/yocto/inference_ort.cpp src/yocto/CMakeLists.txt
git commit -m "feat(yocto): implement the ONNX Runtime CPU inference backend"
```

---

### Task 6: Wire ORT into the Yocto dispatcher

**Files:**
- Modify: `src/yocto/inference_yocto.c` (hook declarations ~line 100-130, `resolve_auto()` ~line 139, and the backend switch)

**Interfaces:**
- Consumes: the seven `alp_inference_ort_*` symbols from Task 5.
- Produces: `ALP_INFERENCE_BACKEND_CPU` resolving to ORT when `ALP_SDK_USE_ORT_CPU` is defined.

- [ ] **Step 1: Declare the hooks**

Add a block mirroring the existing `#if defined(ALP_SDK_USE_DEEPX_DXM1)` block, guarded by `#if defined(ALP_SDK_USE_ORT_CPU)`, declaring all seven symbols with the exact signatures from Task 5's Interfaces section.

- [ ] **Step 2: Extend `resolve_auto()` as the LAST arm**

```c
static alp_inference_backend_t resolve_auto(void)
{
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	return ALP_INFERENCE_BACKEND_DEEPX_DXM1;
#elif defined(ALP_SDK_USE_DRPAI_V2N)
	return ALP_INFERENCE_BACKEND_DRPAI;
#elif defined(ALP_SDK_USE_ORT_CPU)
	/* No NPU compiled in: the A55s run the model on CPU via ONNX Runtime.
	 * Deliberately last -- an NPU-bearing SoM must never silently fall to
	 * CPU under AUTO, because that is a 10-100x throughput cliff the caller
	 * did not ask for. */
	return ALP_INFERENCE_BACKEND_CPU;
#else
	return ALP_INFERENCE_BACKEND_AUTO;
#endif
}
```

The ordering is the load-bearing part of this task: CPU must be last.

- [ ] **Step 3: Add the switch arm**

Add `case ALP_INFERENCE_BACKEND_CPU:` to the dispatcher's backend switch, routing each operation to its `alp_inference_ort_*` hook, guarded by `#if defined(ALP_SDK_USE_ORT_CPU)`. Match the DEEPX arm's structure exactly.

- [ ] **Step 4: Update the file's header comment**

The header currently states `ALP_INFERENCE_BACKEND_CPU -> TFLM reference kernels on the A55s. Wiring deferred to v0.4`. That is now false in two ways: the wiring is done, and the implementation is ONNX Runtime, not TFLM. Rewrite that line to say so. Also fix the `resolve_auto()` comment reading "CPU TFLM lands v0.4".

Leaving stale prose next to the code it describes is the exact defect class this repo's own gate docstring rule exists to prevent.

- [ ] **Step 5: Verify the build in both configurations**

```bash
cmake -B build/yocto-ort -S . -DALP_OS=yocto -DALP_SDK_USE_ORT_CPU=ON && cmake --build build/yocto-ort --parallel
cmake -B build/yocto-plain -S . -DALP_OS=yocto && cmake --build build/yocto-plain --parallel
```

Expected: both clean. The second proves the `#if` guards still compile with ORT absent.

- [ ] **Step 6: Commit**

```bash
git add src/yocto/inference_yocto.c
git commit -m "feat(yocto): resolve ALP_INFERENCE_BACKEND_CPU to ONNX Runtime"
```

---

### Task 7: Documentation, gates, and PR

**Files:**
- Modify: `metadata/libraries/README.md`, `docs/recommended-libraries.md`, `CHANGELOG.md`, `VERSIONS.md`

- [ ] **Step 1: Update the curated-set surfaces**

Add an `onnxruntime` row to the `metadata/libraries/README.md` table and to the `docs/recommended-libraries.md` class table under an inference category. Do not restate a total library count — that framing was removed in PR #1237 precisely because it drifts.

- [ ] **Step 2: Update VERSIONS.md**

`VERSIONS.md:444` carries `- **Signal:** ARM Compute Library bindings.` as a bare backlog bullet with no ADR and no manifest. ONNX Runtime does not implement it — ACL would be consumed as an ORT execution provider, which is a separate decision. Leave that line alone; add a new bullet recording that the CPU backend now exists.

- [ ] **Step 3: Add the CHANGELOG entry**

One entry under `## [Unreleased]`, stating what landed and — explicitly — that the ORT backend has **not** run on silicon and defaults `OFF`.

- [ ] **Step 4: Run the full local gate set**

```bash
bash scripts/test-all.sh --target dev
```

Expected: `SUITE_EXIT=0`. Capture the exit code directly — do **not** pipe the run through `tail`, which reports the pipe's status and has masked real failures on this repo before.

- [ ] **Step 5: Open the PR**

Follow `alp-lab:opening-github-prs-and-issues`: base `dev`, fill every template section, labels `enhancement` + `area:build` + `area:metadata` + `area:docs`. State plainly in the Test plan that the ORT backend is build-verified only and bench-unverified.

- [ ] **Step 6: Do not merge past a self-caused red**

Triage each CI failure as yours vs base-baseline per the skill's §2 before reacting.

---

## Self-Review

**Spec coverage:** Task 1 covers the unpinned `madgwick-ahrs` defect. Tasks 2–7 cover the Yocto CPU stub. The Ethos-U65 stub and the DEEPX/DRP-AI bench verification are explicitly deferred to separate plans in the Scope Split, not silently dropped.

**Placeholder scan:** Two intentional value-holes remain — `<TAG>`/`<SHA>` in Task 3's manifest, and Task 4's branch on whether an upstream recipe exists. Both are resolved by an explicit command in a preceding step, not left to the implementer's judgement. Task 4 Step 1 says to stop and report rather than improvise if the recipe is absent.

**Type consistency:** The seven `alp_inference_ort_*` signatures in Task 5's Interfaces block are the same names and parameter types declared in Task 6 Step 1 and routed in Task 6 Step 3. `VALID_BLOB_FORMATS` is defined in Task 2 Step 3 and referenced only by Task 2's test. `ALP_SDK_USE_ORT_CPU` is defined in Task 5 Step 3 and guards Task 6 Steps 1–3.

**Known risk, stated not hidden:** Task 5 Step 5 is the largest single step in the plan and is not further decomposable without ORT C API specifics I have not verified against the pinned version. If it proves larger than one sitting, split it at the `open`/`invoke`/`close` boundary — each has its own compile check.
