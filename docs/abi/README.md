@page docs_abi_index ABI snapshots

# Alp SDK ABI snapshots

Each snapshot in this directory is a per-symbol fingerprint of the
public surface declared under `include/alp/` at a specific release
tag.  The fingerprints exist so reviewers can spot accidental ABI
changes between releases without diffing every header by hand.

## Exactly one snapshot is CURRENT; every other one is FROZEN

At any time exactly one file in this directory -- the one named for
the release `metadata/sdk_version.yaml` currently declares (`0.15.0`
-> `v0.15-snapshot.json`) -- is the *working* snapshot: it tracks
`HEAD` and gets regenerated (symbols change, and `generated` bumps to
the date of that change) as the SDK's public headers evolve between
releases. A regen that finds the public surface byte-identical to
what's already committed leaves the file untouched -- `generated`
does not bump on a rerun that changed nothing, so the date means "the
ABI last actually changed", not "someone last ran the gate" (issue
#1232).

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
falls back to `ls docs/abi/v*-snapshot.json | sort -V | tail -1` to
find the CURRENT snapshot -- that selector only ever agreed with the
derived path by coincidence of version-sort ordering, and isn't
protected by the write guard the way the derivation is.

(The ABI freeze gate below also runs `sort -V` over this directory,
but for a narrower question the prohibition above doesn't cover:
ranking the already-FROZEN snapshots against each other *after*
CURRENT has been excluded via this same `metadata/sdk_version.yaml`
derivation. A frozen snapshot's version label is fixed the moment it
freezes and releases only ever ship in increasing version order, so
once CURRENT is off the list, the highest remaining label genuinely
*is* the last release -- there's no separate authoritative "previous
version" pointer it could silently disagree with, unlike CURRENT,
which the write guard exists precisely because it's mutable and can
drift from the derived label. See "A separate, unrelated check"
below.)

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

If the public surface hasn't actually changed, `--output` writes
nothing at all -- the file on disk, including its `generated` date,
is left exactly as committed -- and prints
`docs/abi/<label>-snapshot.json unchanged (ABI identical; generated
date left as-is)` and exits 0. That is the expected, successful
outcome of a no-op regen, not a sign the command failed to run.

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

A separate, unrelated check -- the ABI freeze gate
(`pr-generated-files.yml`'s "ABI freeze gate vs the last released
snapshot" step) -- diffs against the newest FROZEN snapshot on disk
(`ls docs/abi/v*-snapshot.json | sort -V`, excluding the CURRENT
label), not the current snapshot, and blocks only on a `REMOVED`
public symbol (a `CHANGED` entry still prints, but doesn't fail the
step -- pre-1.0 a signature/field change is allowed by
`docs/contribution.md`'s ABI policy either way).

There is a fourth verdict, `MOVED`, which the gate deliberately does
NOT fire on. The diff keys every symbol as `header::symbol`, so
relocating one between public headers -- a header split, or lifting a
hand-written block into its generated `*_routes.h` sibling -- would
otherwise read as `REMOVED` + `ADDED` and block the PR even though no
consumer lost anything. `MOVED` is reported instead when ALL of:

* the symbol name and category are unchanged,
* its recorded hash (value for a macro, full signature for a function,
  normalised body for a typedef) is unchanged, and
* the OLD header still `#include`s the new one, so a translation unit
  including the old header still sees the symbol.

That last condition is reachability and it is proven from the current
tree, not assumed -- without it the verdict would let a genuine removal
pass. Two deliberate restrictions on how it is proven, both erring
toward `REMOVED`:

* **Unconditional includes only.** An `#include` inside an `#if` arm is
  not counted, because the arm may not be taken -- `include/alp/board.h`
  selects between the two carriers' routes headers with mutually
  exclusive arms, and counting either would let a symbol moved out of it
  read `MOVED` while every consumer building the other board really lost
  it. The condition is never evaluated, only its presence.
* **One hop only.** A symbol that moves to a header reachable only
  transitively stays `REMOVED`.

Both failure modes produce a false `REMOVED`, which is noise a human
resolves at review; the opposite error would be a silent ABI break. This used to compare
against a frozen `docs/abi/v0.1-snapshot.json` baseline via `git show
v0.1:...`, gated on a `v0.1` git tag that has never existed and never
will (v0.1 predates the `vX.Y.Z` release-tag convention -- see the (†)
footnote below), so the step always took the "no tag yet" branch and
had never once actually run (issue #996). Comparing against v0.1
turned out not to work as a permanent floor either, even fixed to read
the committed file directly: 12+ releases of legitimate pre-1.0 churn
leave ~200 REMOVED/CHANGED entries against it today, none of them a
real regression. Comparing against the *last released* snapshot
instead answers the question the gate exists for -- did this PR
silently drop a symbol that shipped -- without re-litigating a decade
of already-accepted pre-1.0 evolution.

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
| [`v0.11-snapshot.json`](v0.11-snapshot.json)       | v0.11.0/v0.11.1 | 2026-07-17 | frozen                    |
| [`v0.12-snapshot.json`](v0.12-snapshot.json)       | v0.12.0         | 2026-07-22 | frozen                    |
| [`v0.13-snapshot.json`](v0.13-snapshot.json)       | v0.13.0         | 2026-07-24 | frozen                    |
| [`v0.14-snapshot.json`](v0.14-snapshot.json)       | v0.14.0         | 2026-07-29 | frozen                    |
| [`v0.15-snapshot.json`](v0.15-snapshot.json)       | v0.15.0-rc1 (release candidate; no final `v0.15.0` tag exists yet -- see the note below) | tracks HEAD | **CURRENT** (regenerated by CI/`test-all.sh` until `v0.15.0` ships) |

(†) `v0.1`/`v0.3`/`v0.5` predate `scripts/bump_version.py` and the
`vX.Y.Z` release-tag convention (no `v0.1.0`/`v0.3.0`/`v0.5.0` git tag
exists) -- there is no tagged commit to verify or restore their
content against, so unlike `v0.6`-`v0.9` below they were left as
committed on `dev` rather than force-restored. `v0.8.0` and `v0.8.1`
produced byte-identical public-header fingerprints (the patch release
touched no public header), so `v0.8-snapshot.json` is sourced from
`v0.8.0`, the tag that originally minted the file.

**Note on v0.15.0 (measured 2026-08-02 via `git tag --list`, rechecked
2026-08-07):** the only `v0.15*` tag in the repo is `v0.15.0-rc1`;
there is no plain `v0.15.0` tag.  `metadata/sdk_version.yaml` declares
`version: 0.15.0` / `status: released` -- bumped 2026-07-31 by
`4d0f4aae` for a tag that was never pushed.  This table follows the tag
list, the only artefact that can't drift: it marks
`v0.15-snapshot.json` **CURRENT** (it is still the label
`scripts/abi_snapshot.py --print-current-version` derives from
`sdk_version.yaml`'s `0.15.0`, and its `generated` date still moves
whenever a regen finds real ABI content changed), not
frozen-and-released.  Reconciling `sdk_version.yaml`'s `released`
status against the missing tag is outside this file's ownership; it
resolves when the GA tag lands.

The `CHANGELOG.md` half of that mismatch **is** now reconciled (#1292):
the dated section is titled `[v0.15.0-rc1] - 2026-07-31`, naming the
tag that actually shipped it, and the accumulated work since sits under
`[Unreleased] - v0.15.0 candidate` to be cut as the GA.  So this note
no longer describes the CHANGELOG as claiming a released `[v0.15.0]`.
