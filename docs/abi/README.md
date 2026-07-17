@page docs_abi_index ABI snapshots

# Alp SDK ABI snapshots

Each snapshot in this directory is a per-symbol fingerprint of the
public surface declared under `include/alp/` at a specific release
tag.  The fingerprints exist so reviewers can spot accidental ABI
changes between releases without diffing every header by hand.

## Exactly one snapshot is CURRENT; every other one is FROZEN

At any time exactly one file in this directory -- the one named for
the release `metadata/sdk_version.yaml` currently declares (`0.11.0`
-> `v0.11-snapshot.json`) -- is the *working* snapshot: it tracks
`HEAD` and gets regenerated (`generated` date bumps, symbols change)
as the SDK's public headers evolve between releases.

**Every OTHER snapshot in this directory is frozen the moment the
next release ships, and must never be regenerated again.** A frozen
snapshot's `generated` date and content are fixed at the release tag
that minted it (`git show vX.Y.Z:docs/abi/vX.Y-snapshot.json`) --
restoring one, if it ever drifts, means checking it out from that
tag, never re-running `abi_snapshot.py` against today's headers.
`scripts/abi_snapshot.py --output` refuses to write a snapshot whose
`--version` doesn't match `metadata/sdk_version.yaml`'s current
release for exactly this reason: a baseline that keeps tracking
`HEAD` after it should have frozen makes a real ABI regression
against that release **invisible**, because the baseline moves with
the change that broke it (issue #803).

`scripts/test-all.sh`, `.github/workflows/pr-generated-files.yml`, and
`.github/workflows/pr-abi-snapshot.yml` all derive "the current
snapshot" at run time from `metadata/sdk_version.yaml`, via
`scripts/abi_snapshot.py --print-current-version` (prints the bare
`vMAJOR.MINOR` label; every caller composes
`docs/abi/<label>-snapshot.json` from it) -- rather than a version
hardcoded at the time the gate was written, which is exactly what let
a past release cut leave the gate silently regenerating an
already-frozen snapshot against `HEAD` (issue #803), and then let the
next release cut leave the hardcoded literal pointing at the
now-frozen PREVIOUS snapshot (issue #826). None of the three ever
falls back to `ls docs/abi/v*-snapshot.json | sort -V | tail -1` --
that selector only ever agreed with the derived path by coincidence of
version-sort ordering, and isn't protected by the write guard the way
the derivation is.

`pr-generated-files.yml` also fails loudly, before it would otherwise
regenerate anything, if the version `metadata/sdk_version.yaml`
declares has no committed `docs/abi/v<N>-snapshot.json` yet -- a
release that bumped the version without adding the new snapshot would
otherwise pass silently, because the regen step just creates the
missing file and `git diff` never reports on an untracked one. That
missing-snapshot gate, together with the write-guard above (which
rejects `--output` for any label other than the current release), is
what turns a missed snapshot bump into a loud CI failure instead of a
repeat of the same silent corruption.

## How a snapshot is generated

```
VERSION=$(python3 scripts/abi_snapshot.py --print-current-version)
python3 scripts/abi_snapshot.py \
    --version "$VERSION" \
    --output "docs/abi/${VERSION}-snapshot.json"
```

The script (see `scripts/abi_snapshot.py`) walks every header,
extracts function declarations, typedefs, and `#define`s, and emits
a JSON document with a SHA-256 short fingerprint per symbol.
`--output` refuses to write a snapshot labelled anything other than
the current release `metadata/sdk_version.yaml` declares -- e.g.
`--version v0.1` now exits 2, because `v0.1` is a FROZEN historical
label, not today's current snapshot (see above).

## How a PR uses a snapshot

The three callers above (`test-all.sh`, `pr-generated-files.yml`,
`pr-abi-snapshot.yml`) all diff against the DERIVED current snapshot:

```
VERSION=$(python3 scripts/abi_snapshot.py --print-current-version)
python3 scripts/abi_snapshot.py --diff "docs/abi/${VERSION}-snapshot.json"
```

Pre-1.0 the diff is informational — additive changes are allowed
between minor releases (per `docs/contribution.md`'s ABI policy).
The diff still highlights surprises (an unintentional rename, a
silent macro change) so they get caught at review time.

A separate, unrelated check -- the v0.1 ABI-floor gate
(`pr-generated-files.yml`'s "ABI freeze gate vs tagged v0.1" step) --
diffs against the frozen `v0.1` baseline specifically, not the current
snapshot: `python3 scripts/abi_snapshot.py --version v0.1 --diff
.abi-baseline/v0.1-snapshot.json`. That gate asserts the public surface
never regresses below the v0.1 floor; it is not "a PR uses a snapshot"
in the sense above.

Post-1.0 a per-release CI workflow (`pr-abi-snapshot.yml`, ships
in v1.0) gates on the diff: any `REMOVED` or `CHANGED` entry
requires a major-version bump.  `ADDED` entries always pass.

## What the snapshot is *not*

- **Not a full C99 parser.**  The script handles the SDK's own
  declaration style (one decl per logical line, no macro-generated
  symbols, no template / generic types).  Adding an exotic header
  to the SDK that the script can't parse is a sign the header is
  too clever for the SDK's audience.
- **Not a substitute for code review.**  ABI compatibility is
  necessary but not sufficient — semantic changes (a function that
  starts returning a new error code, an `alp_pixfmt_t` that grows a
  new enum value the caller's switch doesn't handle) still need
  human eyes.

## Versions on file

| Snapshot                                          | Tag             | Date       | Status                    |
|----------------------------------------------------|-----------------|------------|---------------------------|
| [`v0.1-snapshot.json`](v0.1-snapshot.json)         | v0.1            | 2026-05-10 | frozen (†)                |
| [`v0.3-snapshot.json`](v0.3-snapshot.json)         | v0.3            | 2026-05-13 | frozen (†)                |
| [`v0.5-snapshot.json`](v0.5-snapshot.json)         | v0.5            | 2026-06-06 | frozen (†)                |
| [`v0.6-snapshot.json`](v0.6-snapshot.json)         | v0.6.0          | 2026-06-06 | frozen                    |
| [`v0.7-snapshot.json`](v0.7-snapshot.json)         | v0.7.0          | 2026-06-12 | frozen                    |
| [`v0.8-snapshot.json`](v0.8-snapshot.json)         | v0.8.0 / v0.8.1 | 2026-06-24 | frozen                    |
| [`v0.9-snapshot.json`](v0.9-snapshot.json)         | v0.9.0          | 2026-07-06 | frozen                    |
| [`v0.10-snapshot.json`](v0.10-snapshot.json)       | v0.10.0/v0.10.1 | 2026-07-14 | frozen                    |
| [`v0.11-snapshot.json`](v0.11-snapshot.json)       | v0.11.0         | tracks HEAD | **CURRENT** (regenerated by CI/`test-all.sh` until the next release) |

(†) `v0.1`/`v0.3`/`v0.5` predate `scripts/bump_version.py` and the
`vX.Y.Z` release-tag convention (no `v0.1.0`/`v0.3.0`/`v0.5.0` git tag
exists) -- there is no tagged commit to verify or restore their
content against, so unlike `v0.6`-`v0.9` below they were left as
committed on `dev` rather than force-restored. `v0.8.0` and `v0.8.1`
produced byte-identical public-header fingerprints (the patch release
touched no public header), so `v0.8-snapshot.json` is sourced from
`v0.8.0`, the tag that originally minted the file.
