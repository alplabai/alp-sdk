# WS6-c Phase 1, Batch A — test/util/header-only library manifests

Task: author `metadata/libraries/<canonical-name>.yaml` for 7 legacy
per-core tokens: `etl, fmt, nlohmann_json, doctest, catch2, jsmn, pid`.

## Files created

| Legacy token   | Canonical name  | File                                    |
|----------------|-----------------|------------------------------------------|
| `etl`          | `etl`           | `metadata/libraries/etl.yaml`            |
| `fmt`          | `fmt`           | `metadata/libraries/fmt.yaml`            |
| `nlohmann_json`| `nlohmann-json` | `metadata/libraries/nlohmann-json.yaml`  |
| `doctest`      | `doctest`       | `metadata/libraries/doctest.yaml`        |
| `catch2`       | `catch2`        | `metadata/libraries/catch2.yaml`         |
| `jsmn`         | `jsmn`          | `metadata/libraries/jsmn.yaml`           |
| `pid`          | `pid`           | `metadata/libraries/pid.yaml`            |

Each manifest: `schema_version: 1`, `name`, `description`, `tier: B`,
`version`, `license`, `integration.zephyr.{module, kconfig?}`, plus a
grounding comment header citing the exact source lines (west.yml,
`_LIBRARY_KCONFIG` in `scripts/alp_project_emit/__init__.py`,
`vendors/<lib>/README.md`, `metadata/library-profiles/<lib>/hw-backends.yaml`,
and the teaching-example `testcase.yaml` where one exists). No `requires:`
block on any of the 7 — none has a real capability/core-class/RAM/OS
constraint; each manifest says so in a one-line comment instead of
inventing a `requires:` block.

## Kconfig grounding (verbatim from `_LIBRARY_KCONFIG`)

- `etl` / `fmt` / `doctest` / `nlohmann_json`: the dict's value is a
  `# ... via the v0.4 loader hook` **comment**, not a real `CONFIG_*`
  assignment. The schema's `kconfig` items must match
  `^CONFIG_[A-Z0-9_]+=...$`, so a comment string can't legally go in that
  array. Represented instead as: **no `kconfig:` key at all**, `module:`
  set to the real (disabled-by-default) west project name for etl/fmt/
  doctest (`etl`, `fmt`, `doctest` in `west.yml`'s `extras-cpp` group), or
  `module: null` for `nlohmann_json` (no west entry exists for it at all —
  see concern below). The loader-hook prose lives in the header comment.
- `catch2`: real assignments transcribed verbatim — `CONFIG_ALP_CATCH2_SW=y`,
  `CONFIG_ALP_SDK_CATCH2_VENDORED=y`. `module: Catch2` (matches west.yml's
  real project name casing).
- `jsmn`: `CONFIG_ALP_JSMN_SW=y`. `module: jsmn`.
- `pid`: `CONFIG_ALP_SDK_PID=y`, `CONFIG_ALP_PID_INT_MATH=y`. `module: null`
  (in-tree alp-sdk source, `src/pid.c` + `include/alp/pid.h`, no west
  project).

## Tier decision: all 7 set to `tier: B`

`scripts/validate_metadata.py` cross-checks every `tier: A` manifest against
`metadata/registries/tier-a-library-ci.json`'s `hostBuild.libraries` /
`excludedLibraries` and fails with `"hostBuild: Tier-A libraries missing
from build/exclusion set"` if a Tier-A manifest isn't accounted for there.
5 of these 7 (etl, fmt, doctest, catch2, jsmn) already have a real teaching
example built green on native_sim — a plausible Tier-A case on the ADR 0018
definition — but promoting them would require editing
`metadata/registries/tier-a-library-ci.json`, which is out of scope for
this additive manifest-authoring pass and risks the phase-1/"LOW risk"
boundary the design doc draws. All 7 are `tier: B` here; promotion is a
named follow-up once someone decides which of the 5 CI-proven ones should
join the `hostBuild` matrix.

## Concerns / schema constraints hit

1. **`catch2` license fails schema validation.** catch2's real upstream
   licence is the Boost Software License 1.0 (`vendors/catch2/README.md` +
   `LICENSE.txt`), which is not in `library-v1.schema.json`'s `license`
   enum (`Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, Zlib, MIT-0`). Wrote
   `license: BSL-1.0` (accurate) rather than a false compliant value; the
   manifest documents this and the schema's own description says extending
   the allowlist is "a deliberate human decision (legal review)". **This
   is not novel to my change** — two sibling manifests already landed in
   this same worktree by other Phase-1 batch agents (`minimp3.yaml`:
   `CC0-1.0`, `tinygsm.yaml`: `LGPL-3.0`) hit the identical constraint and
   made the same choice, so `validate_metadata.py`'s repo-wide run already
   shows 3 pre-existing-pattern license failures, not a regression I
   introduced. Needs an orchestrator-level decision: extend the allowlist
   (BSL-1.0, CC0-1.0, LGPL-3.0) after legal review + update
   `metadata/libraries/README.md`'s rationale, in one follow-up change
   covering all three at once.

2. **`nlohmann_json` has no grounded version/west/vendor pin in this
   checkout.** Unlike its four C++ header-only siblings, there is no
   `west.yml` entry and no `vendors/nlohmann_json/` directory. The 4
   in-tree usages (`examples/multicore/{rpmsg-aen,rpmsg-v2n,rpmsg-imx93,
   heterogeneous-offload}/board.yaml`) select it on the Yocto `a32_cluster`
   core, not a Zephyr core, and no meta-alp-sdk OE recipe exists either.
   Used `version: "3.11.3"` (latest known upstream release) as a
   documented best-effort placeholder — flagged in the manifest header as
   NOT a real reproducible pin per ADR 0017, pending a follow-up that adds
   either a real west module or a Yocto recipe.

3. **`pid` is maintainer-authored, not a pinned third-party upstream.**
   `src/pid.c` / `include/alp/pid.h` carry `Copyright 2026 Alp Lab AB` /
   `SPDX-License-Identifier: Apache-2.0` — there is no external release to
   version-pin. Used `version: "0.9.0"` (the alp-sdk release this manifest
   was authored against, per `scripts/alp_cli/__init__.py`), flagged in the
   header as a modeling choice that will drift unless a maintainer process
   updates it alongside `scripts/bump_version.py` (which doesn't currently
   touch library manifests).

## Validation

Ran the schema-only check against exactly these 7 files:

```
python3 -c "import json,yaml,jsonschema; ... [validate against library-v1.schema.json for the 7 files] ..."
```
Result: 6/7 OK (`etl, fmt, nlohmann-json, doctest, jsmn, pid`); `catch2`
fails on `license: 'BSL-1.0' is not one of [...]` (see concern #1).

Also ran the full repo gate `python3 scripts/validate_metadata.py`: all 7
of my files parse and cross-check cleanly except the same catch2 license
enum failure; the run's overall `3 failure(s)` total includes catch2 plus
the two pre-existing sibling manifests (`minimp3`, `tinygsm`) hitting the
identical allowlist gap — not new failures caused by this batch beyond the
one documented above.
