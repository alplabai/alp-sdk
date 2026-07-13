# 0019. Vendor a verbatim e1m-spec pinout snapshot into alp-sdk

Status: Accepted
Date: 2026-07-10

The E1M physical footprint — the fixed 312-pad (E1M) / 496-pad (E1M-X)
geometry, the default function of each pad, and the mechanical envelope —
is the E1M **standard**, owned by the external
[`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec) repo
(`pinout/v1.json`, `pinout/x-v1.json`, `STANDARD.md`). This ADR records a
deliberate, bounded reversal of the previous "nothing from e1m-spec lives
in this repo" rule: alp-sdk now vendors a **verbatim, generated snapshot**
of the machine-readable pinout under `metadata/e1m/`, so the per-SoM
pinout data authored here can be cross-checked against the standard
offline, and so contributors have the authoritative pad numbers in-repo.

## Context

alp-sdk already owns the *per-SoM* pinout layers:

- `metadata/e1m_modules/<family>/*.tsv` — the single source of truth for
  each family's `e1m_pad → e1m_function → silicon_peripheral → silicon_pad`
  routing, hand-authored from the netlists.
- `metadata/pinmux/<family>.yaml` — a generated, byte-gated projection of
  those TSVs (`scripts/gen_pinmux_capability.py`).

What it did **not** have was the standard those TSVs are written against.
A contributor authoring `from-alif.tsv` writes `e1m_pad="A3",
e1m_function="PWM6"` — but to know that A3's standard function *is* PWM6,
they had to open a second repo. Nothing in alp-sdk verified that the pad
ids and function names the TSVs claim actually exist in the E1M standard;
a typo (`PWM6` vs `PWM06`, `A3` vs `A03`) would pass every existing gate
and only surface downstream in alp-studio's pin allocator.

PR #582 tried to close this gap by **hand-transcribing** the 312-pad
footprint from the AEN datasheet into a new `metadata/e1m/footprint.yaml`
(`provenance: web_provisional`). That reintroduced the exact duplication
`docs/e1m-pinout.md` was written to prevent, and created a third,
drift-prone copy of a table that already exists authoritatively upstream
and derived in `metadata/pinmux/*.yaml`.

The real need behind #582 is legitimate — a machine-readable footprint
in-repo for the pin-mux configurator and for gating — but the source must
be the standard, not a hand copy.

## Decision

**Vendor a verbatim snapshot of e1m-spec's machine-readable pinout, and
gate the per-SoM tables against it. Do not hand-transcribe pad data.**

- `scripts/sync_e1m_spec.py` (manual, network-using dev tool) fetches
  `pinout/v1.json`, `pinout/x-v1.json`, and the upstream `loom-v1` schema
  at a pinned e1m-spec commit and writes them **byte-for-byte** to
  `metadata/e1m/pinout-v1.json`, `metadata/e1m/pinout-x-v1.json`, and
  `metadata/schemas/loom-v1.schema.json`, stamping the resolved SHA into
  `metadata/e1m/e1m-spec.lock`. The snapshot is **never** hand-edited.
- `scripts/check_e1m_pinout.py` (offline CI gate) validates each snapshot
  against the vendored `loom-v1` schema, checks the lock matches the
  snapshots, and **reverse-joins** every non-`TBD` `e1m_pad` in
  `metadata/pinmux/*.yaml` against the matching snapshot — requiring the
  row's `e1m_function` to equal the standard pad's `silkscreen`. Today
  that is AEN 96/96 against E1M; V2N (E1M-X) is wired and ready.
- Freshness (is the snapshot behind upstream?) is **not** gated in CI, by
  design — the local-first CI runs offline. Bumping the snapshot is a
  deliberate `sync_e1m_spec.py` commit, reviewed like any other change.

The public SDK contract is unchanged: `<alp/*>` still takes only opaque
integers, and `<alp/e1m_pinout.h>` / `<alp/e1m_x_pinout.h>` remain the only
pad-name surface. The snapshot is **build-input metadata**, not a public
header, and no application or SDK source includes it.

## Consequences

- One source of truth (e1m-spec) with one vendored projection; the TSVs are
  gated against the standard, killing the typo/drift class #582 targeted.
- Contributors edit TSVs with the pad numbers present in-repo — no
  cross-repo lookup.
- A new obligation: when e1m-spec publishes a pinout revision, re-run
  `sync_e1m_spec.py` and land the bump. The lock's SHA makes the pinned
  version auditable.
- `docs/e1m-pinout.md` is updated in the same slice to document the
  snapshot and drop the "no pad data in this repo" framing.
- Supersedes PR #582: no `footprint.yaml`, no `e1m-footprint` schema, no
  hand-transcribed geometry.
