# #610 §7 slice 1 — build-receipt-v1

**Status:** design 2026-07-13
**Scope:** a versioned, deterministic **build receipt** — `build-receipt-v1`
schema + a pure composer that assembles the receipt from already-existing
inputs (SDK source rev, `board.yaml` identity + digest, `alp.lock` digest,
`build-plan-v1`, per-image binary hashes/sizes, toolchain identity). The
deterministic-*packaging* rework (archive normalization, `SOURCE_DATE_EPOCH`)
and full SBOM generation are explicitly later slices; slice 1 references an
existing SBOM path if present but does not generate one.

## Goal

Give a release build a single machine-readable receipt that lets a consumer
verify a bundle corresponds to its source, configuration, tools, and tests —
composed from inputs the SDK already produces (lock, build-plan, provenance
digests), not a parallel truth. Deterministic: identical source + lock +
build-plan + image set ⇒ byte-identical receipt (no wall-clock fields; any
non-reproducible input is named explicitly).

## Non-goals (YAGNI)

- No archive/packaging changes (deterministic tar, `SOURCE_DATE_EPOCH`) — later slice.
- No SBOM *generation* (SPDX/CycloneDX) — the schema carries an optional
  `sbomRef` pointer; producing the SBOM is later.
- No new build step wired into `release.yml` in slice 1 — the composer is a
  standalone script + hermetic tests; CI wiring is a follow-up once a real
  build feeds it.
- No change to `alp.lock`, `build-plan-v1`, or the provenance flow.

## Units

Mirrors alp-lock (#729) / abi_snapshot: pure builder + closed schema + gate.

### 1. `metadata/schemas/build-receipt-v1.schema.json`
Draft 2020-12, `additionalProperties:false`. Root:
`{ schemaVersion:const 1, description, source, config, toolchain, images,
provenance }`.
- `source`: `{ sdkRevision, sdkDirty(bool), appRevision?, appDirty? }`.
- `config`: `{ boardYaml (path), boardYamlDigest (sha256), sku, lockDigest?,
  buildPlanDigest }`.
- `toolchain`: `{ identity (string), compiler?, flags? }` (from build-plan).
- `images`: array of `{ core, path, sha256, sizeBytes }`.
- `provenance`: `{ sbomRef: string|null, attestationRef: string|null }`.

### 2. `scripts/build_receipt.py` — pure composer
No IO beyond reads. `build_receipt(root, build_plan_path, image_paths,
rev_resolver=git) -> dict` — hashes each input, pulls sku/boardYaml from the
build-plan, sdkRevision from git, validates the result against the schema
before returning. `sha256_file(path)`, `digest_json(obj)` helpers.
`MissingInputError` on an absent build-plan/image. stdlib `hashlib`/`json`.

### 3. `scripts/check_build_receipt.py` — schema/self-consistency gate
Validates the committed sample receipt fixture (if any) + the schema itself is
Draft-2020-12-valid + closed. (There is no repo-wide receipt to drift-check yet
— receipts are per-build artifacts; this gate guards the schema + a golden
fixture.)

### 4. Tests — `tests/scripts/test_build_receipt.py` (hermetic)
- schema closed + Draft-2020-12-valid; `schemaVersion const 1`.
- composer over a tmp fixture (fake build-plan.json + two fake image files) →
  receipt validates against schema; image sha256/size correct.
- determinism: two composes of the same inputs are byte-identical
  (`digest_json` equal), and carry NO timestamp field.
- `MissingInputError` on an absent image path.
- dirty-tree flag: a dirty `rev_resolver` sets `source.sdkDirty True`.

## Data format
camelCase (matches alp-lock/build-plan/diagnostic). sha256 as `"sha256:<hex>"`.

## CI wiring
- `check_build_receipt.py` added to the gate set (auto-listed in `catalog.json`).
- No release.yml change in slice 1.

## Acceptance
- [ ] `build-receipt-v1` schema (closed) + composer + gate + hermetic tests.
- [ ] Deterministic (no wall-clock; identical inputs ⇒ identical receipt).
- [ ] Composes only from existing inputs (lock/build-plan/git/images) — no parallel truth.
- [ ] `catalog.json` regenerated; alp.lock regenerated (new schema → schemas digest).

## Follow-ups
- Wire the composer into `release.yml` after a real build produces images.
- Deterministic packaging (archive normalization, `SOURCE_DATE_EPOCH`).
- SBOM generation (SPDX/CycloneDX) feeding `provenance.sbomRef`.
