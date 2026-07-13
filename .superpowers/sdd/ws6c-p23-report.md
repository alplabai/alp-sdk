# WS6-c Phase 2+3 library-resolution consolidation (#610 §6) — report

**STATUS: PARTIAL — landed the safe 40/40 foundation; STOPPED at a hard blocker
before repointing reader-1 / v2 migration / retirement. Emit-parity held at
OK 40/40 through every landed commit. Golden was NOT edited.**

## Commits (branch feat/610-ws6c-phase23, on worktree /home/caner/alp-ws6c-p23-wt)

- `ed485d06` — schema: library-v1 carries `integration.zephyr.hw_backends` (item a)
- `c5fae03e` — fold: 23 `library-profiles/<token>/hw-backends.yaml` → manifest
  `integration.zephyr.hw_backends`, VERBATIM via the alias table (item b)
- `cabc1aaf` — repoint reader-2 (`_emit_library_hw_backends`) to read manifest
  `hw_backends` instead of `library-profiles/` (part of item c)
- `729f3341` — regen `alp.lock` digests

## Final gate results

- `python3 scripts/ws6c_emit_parity.py --check` → **OK: 40/40** (after every commit)
- `scripts/validate_metadata.py` → 0 failures (32 library manifests schema-valid)
- `scripts/check_library_registry.py` → OK
- `scripts/check_emit_snapshots.py` → 29/29 byte-identical
- `scripts/gen_catalog.py --check` → in sync
- `west alp-lock` verify → drift 0
- `pytest tests/scripts/ -q` → **1531 passed, 21 skipped**

## HARD BLOCKER — why reader-1 repoint / v2 migration / retirement did NOT land

Reader-1 is the base-Kconfig table `_LIBRARY_KCONFIG` (consumed by
`kconfig.py::_emit_libraries`). Repointing it to read base Kconfig from the
manifest (item c) is byte-identical for **19 of the 20** per-core library tokens
used across the 40 example cores. Empirically (union rule = manifest
`integration.zephyr.kconfig` ∪ `hw_backends.sw_fallback.kconfig`, dedup) the ONE
drift is:

- **`examples/connectivity/nanopb-encode-decode` / core `m55_hp`**
  - golden: `CONFIG_ALP_NANOPB_SW=y` (only)
  - manifest-derived: `CONFIG_ALP_NANOPB_SW=y` + **`CONFIG_NANOPB=y`**

Root cause: a pre-existing inconsistency in the two hand-maintained tables.
`_LIBRARY_KCONFIG["nanopb"]` uniquely emits ONLY the SDK SW-fallback marker and
OMITS the upstream Zephyr module enable `CONFIG_NANOPB=y` — whereas every other
library (lvgl→`CONFIG_LVGL`, cmsis_dsp→`CONFIG_CMSIS_DSP`, mbedtls→`CONFIG_MBEDTLS`,
…) DOES emit its upstream base. nanopb's manifest correctly carries
`integration.zephyr.kconfig: [CONFIG_NANOPB=y]` (grounded in its header:
"enable symbol CONFIG_NANOPB"). No data field distinguishes nanopb from lvgl —
their manifest+hw_backends shapes are structurally identical — so a principled,
non-special-cased, manifest-derived resolver necessarily emits `CONFIG_NANOPB=y`
for per-core nanopb. That is a **gain of the correct upstream enable**, not a
CONFIG drop, but it is still a byte-level parity failure (39/40).

Per the directive (golden is truth; do not edit it; report the exact drift), I
did NOT edit the golden, did NOT special-case nanopb, and did NOT land the
reader-1 repoint at 39/40. Because `_LIBRARY_KCONFIG` cannot be retired until its
reader is repointed AND parity passes, the dependent work is also held:

- reader-1 base-Kconfig repoint (item c, second half)
- board.schema v2 unified `libraries: [{name, cores?}]` + `m001_to_v2.py` +
  `LATEST=2` + resolver read-both-forms (item d) — the v2 read path routes
  canonical names through reader-1, so it inherits the same blocker
- retirement of `_LIBRARY_KCONFIG`, `_LIBRARY_WEST_MODULES`,
  `_emit_extra_library_profile`'s library-profiles path, the per-core enum in
  board.schema.json, and the `metadata/library-profiles/` tree (item e)

## Recommended human decision (unblocks 40/40)

Regenerate ONLY `tests/fixtures/ws6c-emit-parity/connectivity__nanopb-encode-decode__m55_hp.conf`
to include `CONFIG_NANOPB=y` — a deliberate correctness fix (per-core
`libraries: [nanopb]` now enables the upstream nanopb Zephyr module, matching
every other curated library). With that one golden line accepted, the union-rule
reader-1 repoint reaches 40/40 and items c/d/e can proceed. (Alternatively, if
per-core nanopb must stay SW-only, encode a per-manifest "suppress upstream base
on the per-core path" policy flag — but that re-introduces the bespoke divergence
the consolidation exists to remove.)

## Note on intermediate state

`metadata/library-profiles/*/hw-backends.yaml` is now redundant for curated-library
resolution (reader-2 reads the manifest fold; the content is byte-verified equal).
It is intentionally left in place — its retirement is item (e), gated on the
blocker above. `_emit_extra_library_profile` still reads arbitrary
board.yaml-supplied `profile:` paths (the `extra_libraries:` escape hatch) and is
unchanged.

---

## RESUMED SESSION — nanopb blocker cleared; Phase 2+3 + partial retirement landed

**STATUS: SUBSTANTIALLY COMPLETE. Emit-parity OK 40/40 after every commit;
golden untouched. Two item-(e) deletions deferred (documented below) because
they are load-bearing beyond the consolidation and need a dedicated pass.**

### New commits (branch feat/610-ws6c-phase23)

- `1867f9a7` — reader-1 repoint: per-core `libraries:` base Kconfig now read
  from each manifest's `integration.zephyr.kconfig` ∪ `sw_fallback.kconfig` via
  the alias table (item 1). nanopb now emits `CONFIG_NANOPB=y` matching the
  corrected golden. 40/40.
- `8661d576` — board.schema v2 unified `libraries:[{name,cores?}]` (oneOf with
  v1 bare-string) + `m001_to_v2` byte-faithful migration + `LATEST=2` +
  loader `_normalize_v2_libraries` (reads BOTH v1 per-core and v2 unified) +
  reader-2 annotation stability + migrate/schema-version tests (incl. direct
  m001_to_v2 coverage) (item 2). 40/40.
- `e52983d6` — `west alp-migrate --apply --all`: all 96 board.yaml → v2 (item 3).
  40/40 (resolver reads v2 identically).
- `c192dd3d` — retire `_LIBRARY_KCONFIG` (+ check_template_catalog repoint to the
  alias table); canonicalise west-module resolution and re-key
  `_LIBRARY_WEST_MODULES` (item e, partial).
- `dd4ce15f` — regen `catalog.json` + `alp.lock` (item 5).

### Final gates (all green)

- `ws6c_emit_parity.py --check` → **OK 40/40**
- `pytest tests/scripts/` → **1536 passed, 21 skipped**
- validate_metadata / check_library_registry / check_template_catalog /
  check_emit_snapshots (29/29) / check_board_schema_version (all v2) /
  gen_catalog --check / alp-lock --check / check_quality_registry → all OK

### Two engineering calls worth flagging

1. **reader-2 annotation + west-modules canonicalisation.** Once slices carry
   canonical names (v2), reader-2's cosmetic `# {lib} / {class}` comment and the
   `_LIBRARY_WEST_MODULES` lookup both saw canonical spellings. The comment
   flipped 22 goldens (inline-comment-ONLY, **zero CONFIG delta** — verified) and
   the west name-allowlist silently dropped `cmsis-dsp` (legacy-keyed table).
   Neither is a latent bug like nanopb; both are canonicalisation fallout. Fixed
   in-code to keep emit byte-identical (annotation resolves back to the declared
   token; west lookup canonicalises via the alias table + table re-keyed).
   Golden NOT edited.

### Item (e) deferred deletions (NOT done — load-bearing beyond scope)

- **per-core `libraries` enum in board.schema.json** — KEPT. Removing it breaks
  **76 unit tests** whose inline board.yaml fixtures still use the v1 per-core
  form. Harmless to keep (resolver reads both; all *examples* are v2). Needs a
  fixture-migration pass.
- **`metadata/library-profiles/` tree + `_emit_extra_library_profile`** — KEPT.
  The tree is NOT purely redundant: its per-library config headers
  (`etl_profile.h`, `fmt_config.h`, `doctest_config.h`, `lv_conf.h`,
  `mbedtls_config.h`, `json_config.h`) are load-bearing include dirs in
  `zephyr/CMakeLists.txt:1159-1163`, and `test-all.sh` +
  `lint_doc_yaml_fragments.py` reference the tree. Deleting it wholesale breaks
  the Zephyr build (no local twister gate here to catch it). The
  `extra_libraries: profile:` escape hatch (`_emit_extra_library_profile`) is a
  live user-facing feature with a wide validate.py + ~6-test ripple. Both want a
  dedicated, build-verified change (relocate the config headers, repoint
  CMake/test-all/lint/Kconfig comments/docs).
- **`_LIBRARY_WEST_MODULES` full retirement** — BLOCKED (not just the regression
  fix that landed). Manifests carry `integration.zephyr.module` for every lib,
  but that field is NOT a reliable "is-a-Zephyr-west-project" signal: vendored
  libs (u8g2, catch2, jsmn, …) and header-only C++ libs (etl/fmt/doctest/
  nlohmann-json) all have a `module:` value yet must NOT be west-listed
  (`west update` would reject them). The old 4-entry table was deliberately
  conservative. Cleanly retiring it needs a manifest field distinguishing
  upstream Zephyr modules — a maintainer decision (nanopb-style).
