# ONNX Runtime Own-Recipe Implementation Plan

> **STATUS — LANDED (partial).** `meta-alp-sdk/recipes-devtools/onnxruntime/onnxruntime_1.28.0.bb` exists at the `v1.28.0` pin this plan chose, and the `inference_ort.cpp` backend + dispatcher wiring landed behind `ALP_SDK_USE_ORT_CPU` (PR #1263). The plan's in-scope `E1M-NX9101` enablement did not land: the option defaults **OFF** everywhere and no board/metadata turns it on for `E1M-NX9101`. Kept for implementation-history context.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. (Superseded by the status banner above -- do not execute without reading it first.)

**Goal:** Give the Cortex-A55 SoMs a real `ALP_INFERENCE_BACKEND_CPU` implementation by packaging **upstream** ONNX Runtime in `meta-alp-sdk` at a single version, enabled on `E1M-NX9101` first and extended to the E1M-X family once proven.

**Architecture:** `src/yocto/inference_yocto.c` dispatches to per-backend hook sets (`alp_inference_<backend>_*`), each compiled behind an `ALP_SDK_USE_<BACKEND>` CMake option, with `resolve_auto()` choosing priority. A new `src/yocto/inference_ort.cpp` supplies that contract for ONNX Runtime. A new `meta-alp-sdk` recipe builds upstream ORT so one version serves every family we enable, rather than inheriting a different vendor fork per family.

**Tech Stack:** ONNX Runtime (MIT), Yocto/BitBake (`meta-alp-sdk`, `LAYERSERIES_COMPAT` kirkstone scarthgap), CMake, C++17, Python 3, pytest.

## Global Constraints

- License must be in `metadata/schemas/library-v1.schema.json`'s enum. ONNX Runtime's repo license is **MIT** (verified via the GitHub license endpoint), but see Task 2 — NXP's recipe declares `LICENSE = "MIT & Apache-2.0"` for the same project, because vendored third-party components inside the tree carry Apache-2.0. Resolve this from the actual tree, not from the GitHub badge.
- ADR 0018 tiers are **A** and **B** only. This lands as **B** (recipe-only, not built in alp-sdk per-PR CI).
- ADR 0018 non-goals forbid in-tree vendored library sources. The recipe fetches upstream; **no `vendors/onnxruntime/`**.
- Pins are a tag or a full SHA, never a floating branch — see the `extras-tier1` breakage this repo just repaired (PR #1246).
- `alp.lock`'s `digests.metadata` covers every file under `metadata/`; ANY manifest edit, including comment-only, invalidates it. Regenerate and stage in the same commit.
- Branch off `dev`, PR to `dev`, full `bash scripts/test-all.sh --target dev` green before `gh pr create`. Capture the exit code directly — never pipe the suite through `tail`.
- `bitbake` is NOT available on the primary dev host. Every recipe task below must be executed where BitBake runs (WSL / the build box). Do not mark a recipe task complete on a host that cannot parse it.

---

## Read This First — Why This Is Its Own Plan, And What Could Kill It

The parent plan (`2026-08-05-yocto-cpu-inference-and-ai-library-gaps.md`) assumed
Task 4 would be a small `.bbappend` over an upstream recipe. It is not. Verified:

- `openembedded-core` and `meta-openembedded` — the only layers
  `meta-alp-sdk/conf/layer.conf` declares in `LAYERDEPENDS_alp-sdk` — contain
  **no** ONNX Runtime recipe at all.
- The two vendor stacks that do ship one are **forks, not upstream**:
  `nxp-imx/meta-imx` builds `gitsm://github.com/nxp-imx/onnxruntime-imx.git`
  (NXP fork, carries the `neutron` and `vsinpu` execution providers) at 1.24.3;
  `renesas-rz/meta-renesas-ai` ships 1.8.0.

**The three risks that could sink this plan, stated before any task:**

1. **ORT is a heavy build with recursive submodules.** NXP fetches with `gitsm://`
   for a reason. Expect a long first build and significant disk.
2. **Upstream's CMake uses FetchContent at build time.** NXP's recipe sets
   `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF` and adds a `do_configure:prepend` to
   let abseil fetch. **Network access during `do_compile` violates Yocto's
   offline-build model** and will fail on an isolated builder. Task 3 exists
   solely to determine whether we can pin every FetchContent dependency as a
   proper `SRC_URI` entry. If we cannot, this plan stops and the fallback below
   applies.
3. **We own it forever.** Upstream ORT moves fast; each bump is our patch burden.

**Fallback if Task 3 fails:** depend on `meta-imx-ml` for `E1M-NX9101` only, and
declare `metadata/libraries/onnxruntime.yaml` family-scoped to `nxp-imx9` so its
single `version:` field stays true. The E1M ↔ E1M-X swap promise is **not**
affected either way — per `docs/adr/0011-intra-family-portability.md` those are
separate families and were never mutually swappable. A per-family version split
is contract-legal; it is a product/CX compromise, not a doctrine violation.

---

## DECIDED 2026-08-05 — own recipe, not vendor layers

The maintainer chose the own-recipe path. This section records **why**, so a
future reader does not relitigate it from the cost side alone.

**The scaling argument (the maintainer's).** Alp Lab ships more SoCs over time.
Vendor layers are **O(families)** to manage: every new SoC family adds a layer
dependency, a version, and a licensing review, and the version skew compounds —
today it is already 1.8.0 (`meta-renesas-ai`) versus 1.24.3 (`nxp-imx`) across
two families. An own recipe is **O(1)**: a new family is one more
`COMPATIBLE_MACHINE` against the same version.

**The architectural argument, which matters more.** Vendor ORT forks exist
largely to add **execution providers** — NXP's `neutron` and `vsinpu`, and the
equivalents elsewhere. **alp-sdk does not consume ORT's EP mechanism at all.**
NPU dispatch happens one level above, in `src/yocto/inference_yocto.c`'s
`resolve_auto()`, routing to the Ethos-U, DRP-AI and DEEPX backends. So the
capability the forks exist to deliver is precisely the one this SDK bypasses by
design. Inheriting a fork would mean accepting its version skew to gain a feature
our own dispatch layer replaces. ORT's job here is the **CPU floor**, and upstream
serves that as well as any fork.

This is also the ADR 0017 posture read correctly: "ride over the vendor SDK" bars
*reimplementing* vendor drivers. Packaging an upstream project's own sources in a
`.bb` is ordinary Yocto integration — we consume upstream, we do not fork it. The
vendor forks are the thing that would put a fork in our tree.

**The accepted cost, stated plainly.** We own the bumps. `cmake/deps.txt` carries
~45 pinned dependencies and every ORT upgrade re-pins them. That is real recurring
maintenance and it is the price of the O(1) property above. It is accepted, not
overlooked.

**Consequences for this plan:** the fallback above is now the fallback *only* for
a Task 2 failure — and Task 2 has already passed (see below), so it is not
expected to fire. Task 4's manifest carries one `version:` true on every family we
enable.

---

## Scope

**In:** upstream ORT recipe, `E1M-NX9101` enablement, the library manifest, the
`inference_ort.cpp` backend, dispatcher wiring, docs.

**Out (explicitly, each its own follow-up):**
- Extending to E1M-X (`V2N101/102`, `V2M101/102`) — Task 8 sketches it but it
  needs its own verification pass on RZ/V2N.
- Any NPU execution provider. This backend is CPU only. DRP-AI (#1238) and DEEPX
  keep their own backends and keep priority over CPU in `resolve_auto()`.
- KleidiAI. NXP enables it by default; it is a real Arm micro-kernel win on A55
  and a legitimate follow-up, but it is not needed to close the CPU stub.

---

## File Structure

| File | Responsibility |
|---|---|
| `meta-alp-sdk/recipes-devtools/onnxruntime/onnxruntime_<ver>.bb` | **Create** — builds upstream ORT for aarch64 |
| `metadata/libraries/onnxruntime.yaml` | **Create** — ADR 0018 manifest, tier B, Yocto-only |
| `src/yocto/inference_ort.cpp` | **Create** — the seven `alp_inference_ort_*` hooks |
| `src/yocto/CMakeLists.txt` | **Modify** — `ALP_SDK_USE_ORT_CPU` option + conditional source |
| `src/yocto/inference_yocto.c` | **Modify** — hook declarations, switch arm, `resolve_auto()`, header prose |
| `scripts/alp_model/manifest.py` | **Modify** — add `onnx` to the blob-format set |
| `tests/scripts/test_library_layer.py` | **Modify** — manifest shape test |
| `CHANGELOG.md`, `docs/recommended-libraries.md`, `metadata/libraries/README.md` | **Modify** — curated-set surfaces |

---

## Tasks 1 and 2 — ALREADY EXECUTED, 2026-08-05

Both were run before this plan was published. Their answers are below; **do not
re-derive them**, but do re-verify the checksum if you change the tag.

### Task 1 result

| Fact | Value |
|---|---|
| `ORT_TAG` | `v1.28.0` (latest release, published 2026-07-25) |
| `ORT_SHA` | `da9b5e364c465de65c49d91e696cd6485270757f` (2026-07-25) |
| `ORT_LICENSE_MD5` | `0f7e3b1308cb5c00b372a6e78835732d` |
| LICENSE first line | `MIT License` / `Copyright (c) Microsoft Corporation` |
| Third-party notices | `ThirdPartyNotices.txt` present at the tag root |

Note: the computed md5 is **identical** to the one in NXP's `onnxruntime.inc`,
meaning their fork never modified the LICENSE file. Computing it rather than
copying was still correct, but the earlier caution that it "may not match" turned
out unfounded — record the fact, not the worry.

**Open item for Task 4:** `ThirdPartyNotices.txt` exists, and NXP declares
`LICENSE = "MIT & Apache-2.0"` for the same project. The recipe's `LICENSE` field
should reflect that compound reality. `metadata/libraries/library-v1.schema.json`'s
`license` enum accepts single SPDX identifiers only — so the manifest can honestly
carry `MIT` (the project's own license, which is what we redistribute as
`libonnxruntime.so`'s primary terms) **only if** the vendored Apache-2.0
components do not change the effective distribution terms. Confirm with the
maintainer before writing the manifest; do not silently pick `MIT` because it fits
the enum.

### Task 2 result — offline build is ACHIEVABLE

**Submodules: only three**, and two are irrelevant to an aarch64 Linux CPU build:

```
cmake/external/onnx                     -> https://github.com/onnx/onnx.git          (needed)
cmake/external/libprotobuf-mutator      -> google/libprotobuf-mutator               (fuzzing only)
cmake/external/emsdk                    -> emscripten-core/emsdk (branch 4.0.23)    (WASM only)
```

**FetchContent: `cmake/deps.txt` is a fully pinned CSV** — `Name;Url;SHA1`, ~45
entries, every one an immutable archive URL with a SHA1. That is exactly the shape
a Yocto `SRC_URI` + checksum wants, so **NXP's `-DFETCHCONTENT_FULLY_DISCONNECTED=OFF`
is a shortcut, not a necessity.** Point each entry at its pre-fetched copy with
`-DFETCHCONTENT_SOURCE_DIR_<NAME>`.

The full list is NOT all needed. A CPU-only, no-python, no-tests build excludes at
minimum: `coremltools`, `directx_headers`, `cudnn_frontend`, `cutlass`, `dawn`,
`onnx_tensorrt`, `vulkan_headers`, `tensorboard`, `google_benchmark`, `googletest`,
`pybind11`, and every `protoc_win*` / `protoc_mac_universal` / `protoc_linux_x86*`
binary. **Task 3 must determine the actual induced set empirically** — configure
once and read what CMake requests — rather than transcribing all 45.

Two specifics worth knowing before writing the recipe:

- `protoc_linux_aarch64` is a **prebuilt protoc binary**. On a cross-compile we
  want OE's `protobuf-native` instead; do not let the build download a host
  binary it cannot execute (or worse, one it can).
- **`kleidiai;.../kleidiai/archive/refs/tags/v1.20.0.tar.gz`** is already in
  `deps.txt`. The Arm micro-kernel path this plan lists as a follow-up is
  therefore an ORT build flag, not a separate integration — cheaper than the
  follow-up section implies.

**Verdict: proceed to Task 3.** The fallback in "Read This First" is not needed.

---

### Task 1: Resolve the version, license and license checksum *(DONE — see above)*

**Files:** none yet — this task produces the facts Tasks 2–4 transcribe.

**Interfaces:**
- Consumes: nothing.
- Produces: `ORT_TAG`, `ORT_SHA`, `ORT_LICENSE_MD5`, and the resolved SPDX license expression. Every later task uses these verbatim.

- [ ] **Step 1: Pick the tag**

```bash
gh api "repos/microsoft/onnxruntime/releases/latest" --jq '.tag_name, .published_at'
gh api "repos/microsoft/onnxruntime/tags?per_page=6" --jq '.[].name'
```

As of 2026-08-05 the latest release is **`v1.28.0`** (2026-07-25). Prefer it
unless Step 4 shows a cross-compile blocker, in which case drop to `v1.27.1`.
Record the choice and the reason.

- [ ] **Step 2: Resolve the tag to a full SHA**

```bash
gh api repos/microsoft/onnxruntime/commits/<ORT_TAG> --jq '.sha, .commit.committer.date'
```

- [ ] **Step 3: Compute the license checksum from the real tree**

`LIC_FILES_CHKSUM` must be computed, never copied from another layer — NXP's
`0f7e3b1308cb5c00b372a6e78835732d` is for **their fork's** LICENSE file and may
not match upstream at our tag.

```bash
gh api "repos/microsoft/onnxruntime/contents/LICENSE?ref=<ORT_TAG>" --jq '.content' | base64 -d > /tmp/ort-LICENSE
md5sum /tmp/ort-LICENSE
head -3 /tmp/ort-LICENSE
```

- [ ] **Step 4: Determine the real SPDX expression**

NXP declares `LICENSE = "MIT & Apache-2.0"` for this project. Find out whether
that is because of their fork's additions or because upstream's tree vendors
Apache-2.0 components:

```bash
gh api "repos/microsoft/onnxruntime/contents?ref=<ORT_TAG>" --jq '.[].name' | grep -iE "licen|notice|third"
```

If the tree carries a `ThirdPartyNotices.txt` naming Apache-2.0 components, the
recipe's `LICENSE` must say so, and `metadata/libraries/onnxruntime.yaml`'s
single `license:` field must carry the value that is actually true. **If the
honest value is a compound expression the schema's enum cannot express, STOP and
report** — extending that enum is a maintainer legal-review decision
(`metadata/schemas/library-v1.schema.json` + `metadata/libraries/README.md`
change together), not something to slip into this plan.

- [ ] **Step 5: Record the facts**

Write the five resolved values into the plan's task notes (or the PR draft) so
Tasks 2–4 transcribe rather than re-derive. No commit — this task produces facts.

---

### Task 2: Determine whether an offline build is possible

This is the go/no-go gate for the whole plan. Do it before writing a recipe.

**Files:** none — investigation.

**Interfaces:**
- Consumes: `ORT_TAG` from Task 1.
- Produces: a yes/no on offline buildability, plus the list of FetchContent dependencies that must become `SRC_URI` entries.

- [ ] **Step 1: Enumerate the submodules**

```bash
gh api "repos/microsoft/onnxruntime/contents/.gitmodules?ref=<ORT_TAG>" --jq '.content' | base64 -d
```

Count them and note any that are themselves large (protobuf, abseil, onnx, mimalloc are the usual suspects).

- [ ] **Step 2: Find every FetchContent declaration**

```bash
gh api "repos/microsoft/onnxruntime/contents/cmake/deps.txt?ref=<ORT_TAG>" --jq '.content' | base64 -d
```

`deps.txt` is upstream's manifest of externally-fetched dependencies with URLs
and hashes. Each line is something the Yocto build must obtain offline.

- [ ] **Step 3: Decide**

Yocto builds fetch in `do_fetch` and must not touch the network in
`do_compile`. For each `deps.txt` entry, it must be expressible as a `SRC_URI`
entry with a checksum, and the build pointed at the pre-fetched copy (ORT
supports `-DFETCHCONTENT_SOURCE_DIR_<NAME>` for this).

**Write down the verdict explicitly:**
- If every dependency can be pinned → continue to Task 3.
- If any cannot → **STOP.** Apply the fallback from the "Read This First"
  section (depend on `meta-imx-ml`, scope the manifest to `nxp-imx9`) and
  re-plan. Do not proceed by setting `FETCHCONTENT_FULLY_DISCONNECTED=OFF` and
  hoping — that is exactly the network-during-build violation this step exists
  to prevent, and it will fail on an isolated builder after passing on yours.

---

### Task 3: Write the recipe

**Files:**
- Create: `meta-alp-sdk/recipes-devtools/onnxruntime/onnxruntime_<ORT_TAG>.bb`

**Interfaces:**
- Consumes: every fact from Tasks 1 and 2.
- Produces: an `onnxruntime` package providing `libonnxruntime.so` plus the `onnxruntime_c_api.h` header that Task 5 compiles against.

- [ ] **Step 1: Read the house style first**

Read `meta-alp-sdk/recipes-renesas/mera2-drpai-tvm_2.7.0.bb` end to end before
writing. This layer's recipes carry long, specific header comments that record
*why* — what gap the recipe fills, what was tried, what trap it hit. Match that
density. A bare recipe with no header does not match this layer.

- [ ] **Step 2: Write the recipe**

Structure it on what NXP's `onnxruntime.inc` proves is needed, but pointed at
**upstream** and with the dependencies pinned per Task 2:

```
# SPDX-License-Identifier: Apache-2.0
#
# ONNX Runtime -- the CPU inference backend for the Cortex-A55 Linux side.
#
# WHY THIS EXISTS AS OUR OWN RECIPE.  Neither openembedded-core nor
# meta-openembedded ships an ONNX Runtime recipe, and the two vendor
# stacks that do ship FORKS at incompatible versions: nxp-imx builds
# gitsm://github.com/nxp-imx/onnxruntime-imx.git at 1.24.3 (carrying the
# neutron + vsinpu execution providers), meta-renesas-ai ships 1.8.0.
# Inheriting either would make metadata/libraries/onnxruntime.yaml's
# single `version:` field untrue on one family or the other.  This recipe
# builds UPSTREAM at one version so the manifest stays honest.
#
# It does NOT vendor sources (ADR 0018 non-goal) and does NOT fork
# upstream (ADR 0017) -- it is packaging only.

DESCRIPTION = "ONNX Runtime -- cross-platform inference engine"
HOMEPAGE = "https://onnxruntime.ai"
LICENSE = "<from Task 1 Step 4>"
LIC_FILES_CHKSUM = "file://LICENSE;md5=<from Task 1 Step 3>"

SRC_URI = "gitsm://github.com/microsoft/onnxruntime.git;protocol=https;branch=main"
SRCREV = "<ORT_SHA from Task 1 Step 2>"
S = "${WORKDIR}/git"

DEPENDS = "zlib libpng protobuf protobuf-native"

inherit cmake python3native

OECMAKE_SOURCEPATH = "${S}/cmake"

EXTRA_OECMAKE += " \
    -Donnxruntime_BUILD_SHARED_LIB=ON \
    -Donnxruntime_BUILD_UNIT_TESTS=OFF \
    -Donnxruntime_ENABLE_PYTHON=OFF \
    -DCMAKE_BUILD_TYPE=Release \
"
```

`onnxruntime_BUILD_UNIT_TESTS=OFF` and `ENABLE_PYTHON=OFF` diverge from NXP
deliberately: we ship a runtime, not a test suite or Python bindings, and both
add large dependency trees. Add the `FETCHCONTENT_SOURCE_DIR_*` pointers Task 2
determined.

- [ ] **Step 3: Parse-check**

On the BitBake host:

```bash
bitbake -p
bitbake-layers show-recipes onnxruntime
```

Expected: parses, and resolves to our recipe at the pinned version.

- [ ] **Step 4: Build it**

```bash
bitbake onnxruntime
```

This is the long one. Expected: `libonnxruntime.so` and the C API headers land
in the sysroot. If it fails on a fetched dependency, return to Task 2 — do not
paper over it with network access.

- [ ] **Step 5: Commit**

```bash
git add meta-alp-sdk/recipes-devtools/onnxruntime/
git commit -m "feat(yocto): package upstream ONNX Runtime for the A55 SoMs"
```

---

### Task 4: Add the library manifest

**Files:**
- Create: `metadata/libraries/onnxruntime.yaml`
- Test: `tests/scripts/test_library_layer.py`

**Interfaces:**
- Consumes: Task 1's version/license; Task 3's package name.
- Produces: `libraries: [onnxruntime]` selectable from `board.yaml`.

- [ ] **Step 1: Write the failing test**

```python
def test_onnxruntime_is_yocto_only_and_a_class():
    """ORT targets the A55 Linux side only. A zephyr section would imply an
    M-class build we do not ship."""
    manifest = yaml.safe_load(
        (REPO / "metadata" / "libraries" / "onnxruntime.yaml").read_text(encoding="utf-8")
    )
    assert manifest["tier"] == "B"
    assert "yocto" in manifest["integration"]
    assert "zephyr" not in manifest["integration"]
    assert manifest["requires"]["core_class"] == "a"
```

- [ ] **Step 2: Run it, expect FileNotFoundError**

Run: `python3 -m pytest tests/scripts/test_library_layer.py::test_onnxruntime_is_yocto_only_and_a_class -v`

- [ ] **Step 3: Write the manifest**

Follow `metadata/libraries/ros2.yaml` — the existing Yocto-only precedent using
`integration.yocto.image_install`. Header records: the pin from Task 1, that
neither OE layer ships ORT, that the two vendor recipes are forks at 1.24.3 and
1.8.0, and that this manifest deliberately describes the upstream build so its
`version:` is true on every family we enable.

State plainly that ORT is the **CPU** backend and does not displace DRP-AI or
DEEPX — `resolve_auto()` keeps NPUs ahead of it.

- [ ] **Step 4: Run tests + validate + regenerate**

```bash
python3 -m pytest tests/scripts/ -q
python3 scripts/validate_metadata.py
python3 scripts/gen_portability_matrix.py && python3 scripts/gen_portability_matrix.py --check
python3 scripts/west_commands/alp_lock.py --workspace .
```

All must pass; `--check` must report in sync. Stage `alp.lock` with this commit.

- [ ] **Step 5: Commit**

```bash
git add metadata/libraries/onnxruntime.yaml docs/portability-matrix.md alp.lock tests/scripts/test_library_layer.py
git commit -m "feat(libs): curate onnxruntime as a Tier B Yocto library"
```

---

### Task 5: Add `onnx` to the blob-format set

**Files:**
- Modify: `scripts/alp_model/manifest.py:38`, `scripts/alp_model/adapters/__init__.py`
- Test: `tests/scripts/test_alp_model_adapters.py`

**Interfaces:**
- Produces: `"onnx"` as a valid `blob_format`, consumed by Task 6's backend.

- [ ] **Step 1: Write the failing test**

```python
def test_onnx_is_an_accepted_blob_format():
    """The ORT CPU backend consumes raw .onnx -- neither vela_tflite nor dxnn
    nor drpai_dir."""
    from alp_model import manifest
    assert "onnx" in manifest.VALID_BLOB_FORMATS
```

- [ ] **Step 2: Run it, expect AttributeError or AssertionError**

Run: `python3 -m pytest tests/scripts/test_alp_model_adapters.py::test_onnx_is_an_accepted_blob_format -v`

- [ ] **Step 3: Promote the comment to a constant**

`manifest.py:38` documents the set only in a trailing comment
(`# vela_tflite | tflite | drpai_dir | dxnn`). Replace with an enforced constant
so a new backend cannot invent a format string:

```python
# The blob formats the SDK can describe. A real constant, not a comment, so a
# new backend cannot silently invent a format string.
VALID_BLOB_FORMATS = frozenset({"vela_tflite", "tflite", "drpai_dir", "dxnn", "onnx"})
```

Mirror the list in the `Blob` docstring in `adapters/__init__.py`.

- [ ] **Step 4: Run tests, expect PASS**

Run: `python3 -m pytest tests/scripts/ -q`

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_model/manifest.py scripts/alp_model/adapters/__init__.py tests/scripts/test_alp_model_adapters.py
git commit -m "feat(model): accept onnx as a blob_format"
```

---

### Task 6: Implement the ORT backend hooks

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

- [ ] **Step 1: Read the sibling implementation**

Read `src/yocto/inference_drpai.cpp` (made real by #1238) end to end. Same hook
contract, same `extern "C"`-from-C++ pattern, same error mapping. Match its
structure rather than inventing one.

- [ ] **Step 2: Write the skeleton, hooks returning NOSUPPORT**

Create `src/yocto/inference_ort.cpp` with all seven hooks. `open` validates
arguments and returns `ALP_ERR_NOSUPPORT`; the rest return `ALP_ERR_NOSUPPORT`.
This exists so Step 4 proves the build wiring before any ORT API is involved.

```cpp
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ONNX Runtime CPU backend for the Cortex-A55 Linux side.
 *
 * Fills the ALP_INFERENCE_BACKEND_CPU slot, whose header comment in
 * inference_yocto.c has read "Wiring deferred to v0.4" since v0.4.  Lowest
 * priority in resolve_auto(): an NPU-bearing SoM still selects its NPU
 * under AUTO -- this is the portable floor, not a replacement.
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

- [ ] **Step 3: Wire the CMake option**

Mirror `ALP_SDK_USE_DEEPX_DXM1`'s declaration in `src/yocto/CMakeLists.txt`:

```cmake
option(ALP_SDK_USE_ORT_CPU "Build the ONNX Runtime CPU inference backend" OFF)
if(ALP_SDK_USE_ORT_CPU)
	target_sources(alp PRIVATE inference_ort.cpp)
	target_compile_definitions(alp PRIVATE ALP_SDK_USE_ORT_CPU)
	target_link_libraries(alp PRIVATE onnxruntime)
endif()
```

Default `OFF` matches `ALP_SDK_USE_DRPAI_V2N`: a backend that has not run on
silicon does not default on.

- [ ] **Step 4: Verify the wiring compiles**

```bash
cmake -B build/yocto-ort -S . -DALP_OS=yocto -DALP_SDK_USE_ORT_CPU=ON
cmake --build build/yocto-ort --parallel
```

If `onnxruntime_c_api.h` is not found, Task 3 did not stage headers — fix there.

- [ ] **Step 5: Implement for real**

`open`: create `OrtEnv` + `OrtSession` from `cfg->model_data`/`cfg->model_size`
via `CreateSessionFromArray`; cache input/output counts and tensor metadata on
the handle. `invoke`: `OrtApi::Run`. `close`: release session then env, in that
order, tolerating a NULL handle. Map every ORT status onto an `alp_status_t`
using `inference_drpai.cpp`'s existing mapping table — do not invent new mappings.

**Note:** `libalp_sdk` now links with `-Wl,--no-undefined` (#1145). An unresolved
ORT symbol fails at alp-sdk's own link, not in a downstream consumer. That is the
desired behaviour; do not work around it.

- [ ] **Step 6: Rebuild and format**

```bash
cmake --build build/yocto-ort --parallel
git diff -U0 origin/dev -- "*.c" "*.h" "*.cpp" | clang-format-diff.py -p1
```

Build clean; clang-format output empty.

- [ ] **Step 7: Commit**

```bash
git add src/yocto/inference_ort.cpp src/yocto/CMakeLists.txt
git commit -m "feat(yocto): implement the ONNX Runtime CPU inference backend"
```

---

### Task 7: Wire ORT into the dispatcher

**Files:**
- Modify: `src/yocto/inference_yocto.c` (hook declarations ~line 100-130, `resolve_auto()` ~line 139, the backend switch, and the file header)

**Interfaces:**
- Consumes: the seven symbols from Task 6.
- Produces: `ALP_INFERENCE_BACKEND_CPU` resolving to ORT when `ALP_SDK_USE_ORT_CPU` is defined.

- [ ] **Step 1: Declare the hooks**

Add a block mirroring `#if defined(ALP_SDK_USE_DEEPX_DXM1)`, guarded by
`#if defined(ALP_SDK_USE_ORT_CPU)`, with the exact signatures from Task 6.

- [ ] **Step 2: Extend `resolve_auto()` — CPU LAST**

```c
static alp_inference_backend_t resolve_auto(void)
{
#if defined(ALP_SDK_USE_DEEPX_DXM1)
	return ALP_INFERENCE_BACKEND_DEEPX_DXM1;
#elif defined(ALP_SDK_USE_DRPAI_V2N)
	return ALP_INFERENCE_BACKEND_DRPAI;
#elif defined(ALP_SDK_USE_ORT_CPU)
	/* No NPU compiled in: the A55s run the model on CPU via ONNX Runtime.
	 * Deliberately LAST -- an NPU-bearing SoM must never silently fall to
	 * CPU under AUTO, because that is a 10-100x throughput cliff the caller
	 * did not ask for. */
	return ALP_INFERENCE_BACKEND_CPU;
#else
	return ALP_INFERENCE_BACKEND_AUTO;
#endif
}
```

The ordering is the load-bearing part of this task.

- [ ] **Step 3: Add the switch arm**

Add `case ALP_INFERENCE_BACKEND_CPU:` routing each operation to its
`alp_inference_ort_*` hook, guarded by `#if defined(ALP_SDK_USE_ORT_CPU)`,
matching the DEEPX arm's structure.

- [ ] **Step 4: Fix the now-false header prose**

The file header says `ALP_INFERENCE_BACKEND_CPU -> TFLM reference kernels on the
A55s. Wiring deferred to v0.4`, and `resolve_auto()`'s comment says
`CPU TFLM lands v0.4`. Both are now false twice over: the wiring exists, and the
implementation is ONNX Runtime, not TFLM. Rewrite both.

- [ ] **Step 5: Build both configurations**

```bash
cmake -B build/yocto-ort -S . -DALP_OS=yocto -DALP_SDK_USE_ORT_CPU=ON && cmake --build build/yocto-ort --parallel
cmake -B build/yocto-plain -S . -DALP_OS=yocto && cmake --build build/yocto-plain --parallel
```

The second proves the `#if` guards still compile with ORT absent.

- [ ] **Step 6: Commit**

```bash
git add src/yocto/inference_yocto.c
git commit -m "feat(yocto): resolve ALP_INFERENCE_BACKEND_CPU to ONNX Runtime"
```

---

### Task 8: Docs, gates, PR

**Files:** `metadata/libraries/README.md`, `docs/recommended-libraries.md`, `CHANGELOG.md`, `VERSIONS.md`

- [ ] **Step 1: Curated-set surfaces**

Add an `onnxruntime` row to `metadata/libraries/README.md`'s table and to
`docs/recommended-libraries.md`'s class table under an inference category. Do
**not** restate a total library count — that framing was removed in PR #1237
because it drifts.

- [ ] **Step 2: VERSIONS.md**

Add a bullet recording that the A55 CPU inference slot is now real. Leave
`VERSIONS.md:444`'s `- **Signal:** ARM Compute Library bindings.` alone — ACL
would be an ORT execution provider, a separate decision.

- [ ] **Step 3: CHANGELOG**

One `[Unreleased]` entry. State explicitly that the backend defaults **OFF** and
has **not** run on silicon.

- [ ] **Step 4: Full local gates**

```bash
bash scripts/test-all.sh --target dev
```

Expect `SUITE_EXIT=0`. Capture the exit code directly; do not pipe through `tail`.

- [ ] **Step 5: PR**

Per `alp-lab:opening-github-prs-and-issues`: base `dev`, every template section
filled, labels `enhancement` + `area:build` + `area:metadata` + `area:docs`.
State in the Test plan that the backend is build-verified only, bench-unverified,
and enabled on `E1M-NX9101` only.

- [ ] **Step 6: Do not merge past a self-caused red**

Triage each CI failure as yours vs base-baseline before reacting.

---

## Follow-ups (not this plan)

- **Extend to E1M-X** (`V2N101/102`, `V2M101/102`). Needs its own verification on
  RZ/V2N. Only then can the manifest claim both families.
- **KleidiAI execution provider.** NXP enables it by default; real A55 win.
- **The ACL signal** at `VERSIONS.md:444` — most naturally an ORT execution
  provider now, which is a cheaper path than standalone ACL bindings. Worth
  re-scoping that backlog line once this lands.

## Self-Review

**Spec coverage:** the parent plan's Tasks 2–7 are all represented (blob_format →
Task 5, manifest → Task 4, recipe → Task 3, backend → Task 6, dispatcher → Task 7,
docs → Task 8), plus the two tasks the parent lacked: version/license resolution
(Task 1) and the offline-build go/no-go (Task 2).

**Placeholder scan:** `<ORT_TAG>`, `<ORT_SHA>`, `<ORT_LICENSE_MD5>` and the SPDX
expression are all resolved by explicit commands in Task 1 before any file uses
them. Task 2 is a genuine decision point with a stated stop condition and a
stated fallback, not a deferred TODO.

**Type consistency:** the seven `alp_inference_ort_*` signatures in Task 6's
Interfaces block are the names and types declared in Task 7 Step 1 and routed in
Task 7 Step 3. `VALID_BLOB_FORMATS` is defined in Task 5 Step 3.
`ALP_SDK_USE_ORT_CPU` is defined in Task 6 Step 3 and guards Task 7 Steps 1–3.

**Known risks, stated not hidden:** Task 2 may end the plan — that is its
purpose. Task 3 Step 4 (the first full build) and Task 6 Step 5 (the real ORT
API) are the two largest steps; Task 6 Step 5 splits at the
`open`/`invoke`/`close` boundary if it runs long, each with its own compile check.
