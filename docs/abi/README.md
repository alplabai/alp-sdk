@page docs_abi_index ABI snapshots

# Alp SDK ABI snapshots

Each snapshot in this directory is a per-symbol fingerprint of the
public surface declared under `include/alp/` at a specific release
tag.  The fingerprints exist so reviewers can spot accidental ABI
changes between releases without diffing every header by hand.

## How a snapshot is generated

```
python3 scripts/abi_snapshot.py \
    --version v0.1 \
    --output docs/abi/v0.1-snapshot.json
```

The script (see `scripts/abi_snapshot.py`) walks every header,
extracts function declarations, typedefs, and `#define`s, and emits
a JSON document with a SHA-256 short fingerprint per symbol.

## How a PR uses a snapshot

```
python3 scripts/abi_snapshot.py --diff docs/abi/v0.1-snapshot.json
```

Pre-1.0 the diff is informational — additive changes are allowed
between minor releases (per `docs/contribution.md`'s ABI policy).
The diff still highlights surprises (an unintentional rename, a
silent macro change) so they get caught at review time.

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

| Snapshot                                         | Tag   | Date       |
|--------------------------------------------------|-------|------------|
| [`v0.1-snapshot.json`](v0.1-snapshot.json)       | v0.1  | 2026-05-10 |
| [`v0.3-snapshot.json`](v0.3-snapshot.json)       | v0.3  | 2026-05-13 |
