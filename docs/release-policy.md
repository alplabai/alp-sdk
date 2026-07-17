# Release policy

How the Alp SDK ships versions, what we promise post-1.0, and how
deprecations work.

This doc is paired with [`VERSIONS.md`](../VERSIONS.md) (the
versioned roadmap) and [`docs/test-plan.md`](test-plan.md) (the
verification ledger that gates each tag).

## SemVer commitment

Post-1.0 the SDK follows [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html):

| Bump      | Allowed changes |
|-----------|-----------------|
| **MAJOR** | Breaking ABI / API change.  Removed public symbols, signature changes, behavioural changes that break the documented contract. |
| **MINOR** | Backwards-compatible additions: new public symbols, new optional `board.yaml` blocks, new chip drivers, new `<alp/X.h>` headers, new opt-in Kconfigs. |
| **PATCH** | Bug fixes that preserve every documented contract.  No new public symbols. |

### What counts as "breaking" for the SDK

The SDK has two consumer surfaces; both are SemVer-protected:

1. **C/C++ ABI** — every symbol in `include/alp/*.h`.  Verified by
   `scripts/abi_snapshot.py` against the most recent
   `docs/abi/v<N>-snapshot.json`.  Tracked at PR time via
   `.github/workflows/pr-abi-snapshot.yml` post-1.0.
2. **`board.yaml` schema + CLI contracts** — `metadata/schemas/*.json`
   and `scripts/{alp_project,validate_board_yaml,program_eeprom}.py`
   exit codes and stdout shapes.

Removing **any** `[ABI-STABLE]`-marked symbol from the public
headers requires a major bump.  Adding new ones is minor.
Renaming a symbol counts as remove + add — major.

`[ABI-EXPERIMENTAL]`-marked symbols are explicitly excluded from
the SemVer contract; they may change in any release.  Promoting
them to `[ABI-STABLE]` is a minor bump.

## Pre-1.0 cadence

- ~6 weeks between minor releases (v0.1 → v0.2 → v0.3 → v0.4 → v0.5).
- Patch releases as bugs warrant; no fixed cadence.
- ABI may change between minor pre-1.0 releases — every minor
  release ships a refreshed `docs/abi/v<N>-snapshot.json`.
- v1.0 tags when v0.4 has been stable for 8+ weeks with real
  users (per `VERSIONS.md` release cadence).

## Post-1.0 LTS cadence

- **v1.0 is the first LTS release.**  Supported for **24 months**
  from tag date.  "Supported" means: security patches + critical
  bug fixes get backported to `release/v1.0` and tagged as
  `v1.0.x`.
- Minor releases on `main` continue every ~6 weeks
  (v1.1 / v1.2 / ...).  Each may be promoted to LTS if scope
  justifies; otherwise the prior LTS keeps support.
- A new LTS supersedes the prior LTS 6 months after its tag;
  customers have a 6-month overlap window to migrate.

## Deprecation procedure

When a public symbol or `board.yaml` field needs to go away:

1. **Soft-deprecation announcement** (no version bump).  The symbol
   keeps working.  Marker added:
   ```c
   /** [DEPRECATED v1.2] Use alp_foo_v2 instead.
    *
    *  Will be removed in v2.0.0.  See release-policy.md. */
   alp_status_t alp_foo(...);
   ```
   CHANGELOG gets a `### Deprecated (v1.2)` section row.
2. **6-month minimum** between soft-deprecation and removal.  This
   is a hard floor; longer is fine.
3. **Removal in the next major bump.**  Same release that removes
   it cannot also break something else — every breaking removal
   gets at least one minor release of overlap with the v2 surface.

`board.yaml` schema-level deprecations follow the same procedure
but the marker is a JSON-Schema `deprecated: true` field.

## Branch model

Summary table; full topology + PR rules + merge methods +
branch-protection settings are in
[`docs/branching-and-merge-policy.md`](branching-and-merge-policy.md):

| Branch                  | Purpose                                          | Push policy             |
|-------------------------|--------------------------------------------------|-------------------------|
| `main`                  | Tested, releasable baseline; tags are cut here.  | PR only; branch-protected. |
| `dev`                   | Shared integration branch for untested work; promoted to `main` at the release gate (after bench/HW/CI). | PR only. |
| `release/v1.0`          | LTS; cherry-picks of security + critical fixes.  | PR only; signed commits required. |
| `release/v1.1`, ...     | Next minors after they ship; same LTS rules if promoted. | PR only; signed commits required. |
| Feature branches        | `feat/<topic>` / `fix/<topic>` / `docs/<topic>`; branch off `dev`, merged back into `dev` via PR (`--no-ff`). | OK on contributor forks/branches. |

Tags on `main` are `v0.5.0`, `v0.6.0`, `v1.0.0`, ... — cut on `main` once
`dev` has been promoted through the release gate.  Tags on
`release/v1.0` are `v1.0.0`, `v1.0.1`, ... — incremented on each
LTS-branch release.

## Release-cut procedure

Codified in `.github/workflows/release.yml` + `scripts/bump_version.py`.
The cut happens on `main`, and only after `dev` has been promoted to
`main` through the release gate (bench / hardware / CI passed):

1. Validate the verification ledger -- every row in
   `docs/test-plan.md` for the target version must be `✅` or `n/a`.
2. Regenerate `docs/abi/v<N>-snapshot.json` for the target
   version.  `pr-abi-snapshot.yml` (post-1.0) gates the diff.
3. Update `metadata/sdk_version.yaml` to the new version.
4. Slice the `## [Unreleased]` section of `CHANGELOG.md` into
   `## [v<N>] - YYYY-MM-DD`.
5. Tag: `git tag -s v<N>` (signed).
6. Push tag; the release workflow auto-creates the GitHub Release
   with the CHANGELOG slice as the body and the source tarball
   (`alp-sdk-v<N>.tar.gz`) plus three sidecar files as artefacts:
   SHA-256 + SHA-512 checksums, and a SLSA v1.0 Build **Level 3**
   provenance attestation (`alp-sdk-v<N>.tar.gz.intoto.jsonl`).
   The attestation is produced by the
   `slsa-framework/slsa-github-generator` reusable workflow
   running in an isolated, ephemeral GitHub-hosted runner that
   the `build` job cannot tamper with -- the L3 promise.
   Verifiable with
   `gh attestation verify alp-sdk-v<N>.tar.gz --owner alplabai`;
   the command checks the Sigstore signature, the workflow
   identity (`slsa-framework/slsa-github-generator`), the source
   repo, and the artefact digest all match in a single check.

A patch release skips steps 1-2 (the verification ledger doesn't
move; ABI doesn't change) but still updates `sdk_version.yaml`
+ CHANGELOG slice.

## Security backports

Critical CVEs land via `release/v1.0` even after a newer LTS
supersedes it, until the 24-month support window expires.  See
[`docs/security-advisories.md`](security-advisories.md) for the
embargoed-disclosure workflow.

## What's out of scope

This doc covers the **SDK** repo (`alp-sdk`).  It does NOT cover:

- `alp-studio` (separate repo, separate SemVer track).
- `alp-sdk-internal` (private; not SemVer'd — schema changes
  coordinate with this repo's MAJOR bumps).
- `e1m-spec` (the hardware standard; tracked separately as v1.0 /
  v1.1 / etc.).
