# 0022. `tan` ships as Python, not Rust; Renode is retired from the command surface

Status: Accepted in part — amended 2026-08-07. The Python-executor clause
SHIPPED and stands (`tan` v0.5.0+ is a PyInstaller freeze of the Python
package `alp-tan`). **The Renode-retirement clause did NOT ship and is
WITHDRAWN** — see [Amendment 1](#amendment-1--2026-08-07-the-renode-retirement-clause-is-withdrawn).
`tan renode` is still a live, registered verb; Decision points 2 and 3 below
are withdrawn, and the doc removals they authorised are being reverted.
This ADR retired the `tan renode` VERB only. alp-sdk's own Renode CI is untouched
and still runs: `pr-renode-aen-smoke`, `pr-renode-dual-os`, `pr-renode-sim-mode`
and `pr-renode-v2n-sci0-smoke` are all active workflows, and
`pr-renode-aen-smoke` installs the pinned Renode v1.16.1 and boots a real image
with `renode --console --disable-xwt --plain` — no `tan renode` involved, which
is exactly why retiring the verb costs alp-sdk no simulator coverage. The models
under `metadata/renode/*.repl` / `*.resc` are retained for the same reason. Only
the three `--sim-mode` e2e STEPS inside `pr-renode-sim-mode` are no-ops.
Date: 2026-08-04 (Caner)
Deciders: alpCaner (alp-sdk)
Supersedes: [0020](0020-sdk-owns-build-execution.md) — narrowly, two clauses
only: (1) the Rust-executor language claim (the "Implemented" banner's
"standalone, public `tan` CLI", Decision point 2's "a NEW standalone repo
(Rust)", and the Consequences bullet "Rust owns process/cancel/streaming");
(2) every `renode` entry in the documented command surface (Decision point
2's verb list `tan build / flash / image / size / renode / clean / ...`, the
Migration section's Phase 2 scope list, and `docs/cli.md` usage examples).
Everything else in 0020 is unchanged and still governs: the plans-vs-executes
split (end-state B), the three-repo boundary (alp-sdk / `tan` / alp-sdk-vscode),
the plan/manifest contract, the version-skew guard, and the parity-gate
machinery in `tests/parity/`.

## Amendment 1 — 2026-08-07: the Renode-retirement clause is withdrawn

The record below is preserved verbatim per `docs/adr/README.md`'s append-only
rule. This amendment states what is no longer true, and why.

**`tan renode` was never removed.** At `tan-cli` tag `v0.5.1` — the current
release, cut after this ADR was accepted — the verb is still registered:

```
python/tan/cli.py:        from tan.commands.renode_cmd import renode
python/tan/cli.py:        app.command("renode")(renode)
python/tan/commands/renode_cmd.py    (the implementation, present)
```

**The tracking issue was re-scoped away from removal.** `tan-cli`#448, which
Context point 2 and the Related section name as "the `tan renode` command
removal", is **open** and now reads: *"tan renode: emit a support-paused
warning; retain the command, modules, fixtures and CI models."* Retention with
a warning is the opposite of the removal this ADR assumed would follow.

Consequently:

- The Status banner's "implemented" claim was wrong for this half of the ADR.
  Half of it shipped (Python), half never did (Renode).
- **Context point 2's "There is no `tan renode` verb, and no replacement
  simulator" is false as written** and should be read as withdrawn.
- **Decision point 2 is withdrawn.** `renode` should NOT be stripped from live
  "supported command" listings, because the command is live. The removal it
  authorised was applied in `a39d73e5` and then partially reverted in
  `2b817532`, which is why the doc surface is currently inconsistent:
  `docs/cli.md`, `docs/heterogeneous-builds.md` and `docs/board-config-emit.md`
  document the verb, while `README.md`, `docs/README.md`,
  `docs/troubleshooting.md` and `VERSIONS.md` still omit it. Restoring the
  four is tracked separately rather than folded in here, to keep this change
  bounded to the ADR record — the same bounded-scope discipline the original
  Decision point 1 applied to the `cargo install` paragraphs.
- **Decision point 3 is withdrawn.** A reader who reaches for `tan renode`
  should not be redirected to "build + flash to real hardware" as though the
  verb were gone. The genuine caveat is narrower and survives: there is no
  simulated substitute for the cross-core RPMsg handshake specifically.
- Decision point 4 stands, and its premise improves: `examples/aen/aen-sim-vision`
  is no longer internally incoherent, because the `tan renode` run path it
  depends on still exists.
- The Consequences section's "the accepted architecture record matches what a
  reader can actually install and run" did not hold for the Renode half — this
  amendment is what makes it hold.

What still stands unamended: the Python-executor clause in full, and this ADR's
narrow supersession of ADR 0020's Rust-executor **language** claim. Only the
`renode` command-surface supersession of 0020 is withdrawn.

## Context

ADR 0020 committed `tan` to Rust specifically so it could "own
process/cancel/streaming natively" (0020's *Decision, and why B over A*), and
its command surface — carried into this repo's own docs (`docs/cli.md`,
`README.md`, `docs/heterogeneous-builds.md`, `VERSIONS.md`) and into
`tests/parity/` — included `renode` alongside `build`/`flash`/`image`/`size`/
`clean` as a first-class verb, backed by a real Renode-boot smoke test
(`docs/heterogeneous-builds.md`'s "Renode smoke test", the AEN-focused
`examples/aen/aen-sim-vision` example, and seam-2 of the `tan`↔alp-sdk parity
gate).

Both of those facts changed on the `tan-cli` side:

1. **tan is Python, not Rust.** As of `tan` v0.5.0, the released binary is a
   PyInstaller freeze of the Python package `alp-tan`
   (`python/tan/{commands,core,planner,templates}/` in `alplabai/tan-cli`).
   The original Rust crates are frozen — kept only as the release contract's
   shape and as a read-only oracle, not as the shipping executor.
2. **Renode is retired from the command surface.** There is no `tan renode`
   verb, and no replacement simulator. This alp-sdk-side change tracks
   `tan-cli`#448 (`tan renode` removal), scheduled after `tan` v0.5.0 tags; the
   asset removal on this side (`metadata/renode/*.repl` / `*.resc`) is a
   separate, sequenced piece of work.

Neither change touches the plans-vs-executes split itself: alp-sdk still
emits `--emit build-plan` / `--emit system-manifest` and runs no build; `tan`
is still the sole executor and the whole user command surface, still a
standalone, independently-versioned, public binary a user installs without
the IDE extension. What changed is `tan`'s implementation language, and one
retired verb.

## Decision

Correct the record rather than silently edit ADR 0020 (append-only,
per `docs/adr/README.md`'s own rule):

1. Every reference in this repo's docs to `tan` as "a standalone... Rust
   binary" that describes the *language* fact is stale; `tan` is Python
   (PyInstaller-frozen). Docs that describe *how to install/build* `tan` from
   source (`cargo install`, the Rust-toolchain prerequisite) are a separate,
   larger doc pass and are **not** rewritten by this ADR — flagged for a
   follow-up rather than folded in here to keep this change bounded to the
   ADR record and the Renode claims (alp-sdk#1192's stated scope).
2. `renode` is removed from every live "supported command" listing in this
   repo's docs (`docs/cli.md`, `README.md`, `docs/heterogeneous-builds.md`,
   `docs/README.md`, `docs/board-config-emit.md`, `docs/troubleshooting.md`,
   `VERSIONS.md`'s backlog). Historical mentions — a past release's changelog
   entry, a record of what a verb used to be called — are left alone; they
   were true when written and remain true as history.
3. Where a doc told a reader to run `tan renode` to verify something without
   a board, it now says what to do instead: build + flash to real hardware
   (`tan build` + `tan flash`), or — for a single-image target with no
   cross-core dependency — a headless `native_sim` run via `tan run`. There
   is no simulated substitute for the cross-core RPMsg handshake specifically;
   that must be verified on real hardware now.
4. `examples/aen/aen-sim-vision` is explicitly **not** touched by this ADR.
   Its entire premise is Renode-hosted simulation (frame/audio injection over
   the sim's memory-mapped doorbell registers, `tan renode` as its only run
   path); removing the Renode claim there would leave the example internally
   incoherent, not merely stale. Whether it is reworked, archived, or removed
   is a separate decision outside this ADR's docs/ADR-only scope.

## Alternatives

1. **Edit ADR 0020 in place.** Rejected — `docs/adr/README.md`'s own rule is
   append-only; a decision is corrected by a superseding record, not a silent
   rewrite of history.
2. **Do nothing until `tan-cli`#448 lands.** Rejected — the two facts are
   already false in the accepted record today (`tan` v0.5.0 is already
   Python-shipped), and open issues (alp-sdk#1158, #1188) were actively
   trying to repair Renode support while the retirement decision stood;
   leaving the record stale actively misdirects that work.

## Consequences

**Good**
- The accepted architecture record matches what a reader can actually
  install and run.
- A reader who previously relied on `tan renode` is told what to do instead,
  not just told it's gone.

**Bad / costs**
- `examples/aen/aen-sim-vision`'s incoherence is now recorded but not
  resolved — a real cost, deliberately left to a separate decision.
- The Rust-vs-Python *installation* instructions throughout `docs/cli.md` /
  `README.md` (the `cargo install` / Rust-toolchain paragraphs) are known
  stale but out of this ADR's bounded scope; they remain a follow-up.

## Related

- `metadata/renode/*.repl` / `*.resc` asset removal — sequenced separately,
  not part of this change.
- `tan-cli`#448 — the `tan renode` command removal on the `tan-cli` side,
  scheduled after `tan` v0.5.0 tags.
- alp-sdk#1158, #1188 — Renode-repair issues whose disposition follows from
  this decision (retired, not repaired); re-scoping/closing them is tracked
  outside this ADR.
