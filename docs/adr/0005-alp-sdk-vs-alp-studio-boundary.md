# 0005. alp-sdk vs alp-studio repo boundary

Status: Accepted
Date: 2026-05-10

## Context

ALP Lab ships two adjacent open-source repositories that together
power the E1M™ developer experience:

- [`alplabai/alp-sdk`](https://github.com/alplabai/alp-sdk) — this
  repo.  Public C/C++ surface (`<alp/...>` headers), the per-OS
  backends behind it, chip drivers, chip metadata, and the
  hand-written reference apps under `examples/<peripheral>-<demo>/`.
- [`alplabai/alp-studio`](https://github.com/alplabai/alp-studio) —
  the visual programmer.  Block library, deterministic pin
  allocator, codegen templates, and studio-level project tooling.

The two repositories share infrastructure (chip metadata, ABI
snapshot tooling, the public header surface) but serve genuinely
different audiences.  alp-sdk's audience includes **hand-written
firmware authors** who never touch alp-studio — see
[ADR 0001](0001-wrapper-on-top-of-zephyr.md) and the "Two
consumer paths" section of [`README.md`](../../README.md).

Without an explicit rule about which repo a given new addition
belongs in, the temptation is to land everything in alp-sdk
because that is where the SDK's own contributors already work.
That would gradually pull studio-only artefacts (block manifests,
the pin allocator, codegen templates) into a repository whose
hand-written-firmware audience does not need them and shouldn't
be forced to download them.

The decision needed is: **for any new addition, which repo does
it belong in?**

## Decision

A new artefact lives in **alp-sdk** if it satisfies the
"dual-use" acid test, otherwise it lives in **alp-studio**:

> **Acid test** — Would a hand-written-firmware author ever
> directly use this?  If no → alp-studio.  If yes (or "both
> audiences want it") → alp-sdk.

Concretely:

| Belongs in alp-sdk                       | Belongs in alp-studio                   |
|------------------------------------------|-----------------------------------------|
| `<alp/...>` public headers               | Block manifests (`library/blocks/*`)    |
| Chip metadata (`metadata/socs/*.json`)   | Pin allocator                           |
| Generated capability tables              | Codegen templates                       |
| ABI snapshot tooling                     | Studio-only Kconfig helpers             |
| Per-peripheral examples (hand-written)   | Block-level examples (studio-exported)  |
| ADRs about API design                    | ADRs about studio architecture          |
| `scripts/abi_snapshot.py`,               | `scripts/gen_block_manifest.py`,        |
| `scripts/gen_soc_caps.py` (dual-use)     | `scripts/studio_codegen_*` (studio-only)|

The metadata + ABI tooling is single-source-of-truth and lives in
alp-sdk because that is where it is *generated from*; alp-studio
*consumes* it.

## Alternatives

**A. Merge alp-studio into alp-sdk.**  Rejected because it forces
hand-written-firmware authors to download the codegen, the
visual-programmer assets, and the studio-only Kconfig helpers
they will never use.  The SDK is supposed to be a small, focused
firmware dependency; bundling studio inflates it unjustifiably.
This also conflicts with [ADR 0001](0001-wrapper-on-top-of-zephyr.md)
("standalone usage is first-class").

**B. Replicate the shared infra (metadata, ABI tooling, headers)
in both repos.**  Rejected because the metadata files are
single-source-of-truth.  Two copies drift; one becomes stale; the
codegen reads stale data and emits broken C.  The shared infra
lives where it is *generated from*, not where it is consumed.

**C. Keep the boundary informal — let contributors judge.**
Rejected because contributors who haven't read the full project
context default to "land it where the maintainers already work",
which is the gradual-creep failure mode.  An explicit rule + a
table makes the answer cheap to look up during PR review.

## Consequences

**Good:**

- Clean separation of concerns.  Hand-written firmware authors
  get a small SDK they can reason about.  Studio contributors get
  a workspace that contains the visual-programmer code without
  drowning in C headers.
- Contributors and reviewers know where new ideas land.  The
  acid test is one question; the table is one row to look up.
- The metadata + ABI single-source-of-truth lives in one repo.
  No drift.

**Bad / costs:**

- Occasional judgement calls on what "dual-use" means.  Examples:
  *Is a new chip metadata field studio-only?*  If a hand-written
  firmware author would ever read it (e.g. peripheral counts), it
  is dual-use → alp-sdk.  *Is a new block manifest schema
  dual-use?*  Hand-written firmware authors don't write block
  manifests → alp-studio.  Edge cases get a one-paragraph
  rationale in the PR description.
- PR review has to enforce.  Reviewers should reject PRs that add
  studio-only artefacts to alp-sdk and vice versa, pointing at
  this ADR.

## See also

- [ADR 0001](0001-wrapper-on-top-of-zephyr.md) — why the wrapper
  exists and why standalone usage is first-class.
- `docs/architecture.md` — the "Repository boundary" subsection
  surfaces this decision without requiring a click into the ADR.
- [`README.md`](../../README.md) "Two consumer paths" --
  customer-facing framing of the same standalone-vs-studio
  split.
