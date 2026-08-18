# 0022. `tan` ships as Python, not Rust; Renode is retired from the command surface

Status: Accepted — amended twice (2026-08-07, 2026-08-17). The
Python-executor clause SHIPPED and stands (`tan` v0.5.0+ is a PyInstaller
freeze of the Python package `alp-tan`). **The Renode-retirement clause was
WITHDRAWN by Amendment 1 on 2026-08-07 and RE-INSTATED, and widened, by
Amendment 2 on 2026-08-17.** It is in force: Decision points 2 and 3 stand
again, and Decision point 4 is resolved — `examples/aen/aen-sim-vision` is
deleted. Renode leaves alp-sdk entirely, not just the `tan` command surface.
The four `pr-renode-*` workflows — `pr-renode-aen-smoke`,
`pr-renode-dual-os`, `pr-renode-sim-mode`, `pr-renode-v2n-sci0-smoke` — are
DELETED, and with them the hand-maintained Renode v1.16.1 pin and the
`renode --console --disable-xwt --plain` boot that were the whole of
alp-sdk's pre-silicon simulator coverage. `metadata/renode/*.repl` /
`*.resc` and `tests/renode/*` are in scope and go once the `tan-cli` side
lands; they are still present at this commit because `tan-cli`'s
`parity.yml` seam-2 job references those model paths by name. What survives:
the `diagnostics.sim_console` `board.yaml` field, which serves alp-studio's
hardware simulator, not `tan renode`. alp-sdk ships no simulator of its own
and none is planned — the cost is stated plainly in Amendment 2. This block
records CURRENT state and is rewritten in place on each amendment, per the
in-place Status-amendment precedent of ADRs 0006, 0017 and 0020 (and of
Amendment 1's own rewrite, commit `5497a1a5`); the 2026-08-04 and 2026-08-07
wordings it replaces are in git history and quoted where load-bearing in
Amendments 1 and 2. The Decision body below is append-only and stays
verbatim.
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

## Amendment 2 — 2026-08-17: the retirement is re-instated and widened

Amendment 1 withdrew the Renode-retirement clause on 2026-08-07. **That
withdrawal is itself now superseded.** On 2026-08-17 the maintainer re-instated
the retirement and widened it past this ADR's original scope: Renode leaves
alp-sdk, `tan-cli` and alp-sdk-vscode completely, before the pending v0.16.0
(alp-sdk) / v0.6.0 (`tan-cli`) GA releases. Amendment 1's text above is
preserved verbatim per `docs/adr/README.md`'s append-only rule — it is the
record of a withdrawal that held for ten days, not the current state. The order
to read is: accepted 2026-08-04 → withdrawn 2026-08-07 → re-instated and
widened 2026-08-17.

**Withdrawn, then re-instated.** Decision points 2 and 3, withdrawn by
Amendment 1, are back in force. `renode` comes out of every live "supported
command" listing in this repo's docs again, and a reader who reaches for
`tan renode` is again told what to do instead — build + flash to real hardware
(`tan build` + `tan flash`), or a headless `native_sim` run via `tan run` for a
single-image target with no cross-core dependency. The narrower caveat
Amendment 1 salvaged is unaffected and still true: there is no simulated
substitute for the cross-core RPMsg handshake specifically; it must be verified
on real hardware.

**Widened, clause (a): alp-sdk's own Renode CI goes.** Amendment 1's
"alp-sdk's own Renode CI is untouched and still runs" is no longer true. All
four workflows are deleted — `.github/workflows/pr-renode-aen-smoke.yml`,
`.github/workflows/pr-renode-dual-os.yml`, `.github/workflows/pr-renode-sim-mode.yml`,
`.github/workflows/pr-renode-v2n-sci0-smoke.yml` — and with them the hardcoded
Renode v1.16.1 pin each one carries (each job downloads
`renode-1.16.1.linux-portable-dotnet.tar.gz` from the upstream release), along
with the `renode --console --disable-xwt --plain` boot that the pre-2026-08-17
Status block cited as the reason retiring the verb cost alp-sdk no coverage.
Four copies of a pinned third-party emulator version, maintained by hand, is
a standing upgrade tax that only makes sense while the gates are load-bearing.

**Widened, clause (b): the assets go with them — brought into scope here,
deleted in a later commit.** The `metadata/renode/*.repl` / `*.resc` removal
that the Related section below calls "sequenced separately, not part of this
change" is now IN SCOPE of this ADR:
`metadata/renode/alif_ensemble_e8.repl`, `metadata/renode/alif_ensemble_e8.resc`,
`metadata/renode/renesas_rzv2n.repl`, `metadata/renode/renesas_rzv2n.resc`. So
are the Renode-only fixtures under `tests/renode/`:
`aen_m55_itcm_run.overlay`, `aen_m55_sim.conf`, `v2n_m33_ramconsole.conf`,
`v2n_m33_sci0_console.conf`, `v2n_m33_sci0_console.overlay`. With no workflow
and no verb left to consume them, they are hand-maintained models that nothing
exercises — worse than absent, because an unexercised model silently rots into
a wrong description of the silicon.

**All nine files are still PRESENT at this commit**, and that is deliberate,
not an oversight: `tan-cli`'s `.github/workflows/parity.yml` seam-2 job
hard-references those `metadata/renode/` model paths, so deleting them before
the `tan-cli` side lands would red a job in another repo. Authorising the
removal (this amendment) and performing it are two steps here; the deletion
follows the `tan-cli` change. Clause (a) has no such constraint — `tan-cli`
never reads alp-sdk's own workflow files — which is why the four `pr-renode-*`
workflows go in the same commit that resolves Decision point 4, and these nine
do not.

**Decision point 4 is RESOLVED.** That point reserved the disposition of
`examples/aen/aen-sim-vision` to a separate human decision, and Amendment 1
noted its premise had improved because the `tan renode` run path still existed.
That path is retired by this amendment, so the coherence argument cuts the
other way: the whole directory (all 8 files) is **deleted**. An example whose
only run path is a retired verb against retired models cannot be run,
reviewed, or repaired.

**The `tan-cli` side — decided here, landed there.** The `tan renode` verb
WILL BE removed, together with its three modules
(`python/tan/commands/renode_cmd.py`, `renode_plan.py`, `renode_sim.py`) and
its 27 published `renode.*` issue codes; `tan-cli`#448 is re-scoped back from
"retain with a support-paused warning" to removal, which is what this ADR
originally assumed. None of that has happened yet: at the time of writing the
verb is still registered (`app.command("renode", …)` in `python/tan/cli.py`),
all three modules are present, and all 27 codes are still published in
`contract/issue-codes.json`. This amendment is the DECISION; the `tan-cli`
change is a separate repo's commit and lands on its own. Removing published
issue codes shrinks the `envelope-contract.json` release asset — a breaking
change to the published CLI surface, not an additive one, and consumers that
key off the contract (starting with alp-sdk-vscode) will see codes disappear.
That break is deliberately carried by
v0.6.0 / v0.16.0 rather than deferred to a later major: pre-1.0, with the codes
already unemittable once the verb is gone, shipping the shrunk contract
alongside the retirement is better than cutting a GA whose published contract
advertises 27 codes nothing can produce.

**What survives, and why.** The `diagnostics.sim_console` `board.yaml` field is
**retained** everywhere — schema, metadata, emitters and docs. It serves
alp-studio's hardware simulator (issue #686 / alp-studio#74) by emitting
`CONFIG_RAM_CONSOLE=y` for headless cores; that is a studio bundle concern, not
a `tan renode` one, and nothing about this amendment makes it stale. Only one
sentence changes: the `RENODE_MODE=real` reference in its description in
`metadata/schemas/board.schema.json` is reworded, because that env var names
the studio simulator's mode and must no longer read as a pointer to a retired
alp-sdk capability. The historical record is likewise untouched, exactly as
Decision point 2 already carves out — ADR 0010's Renode references, ADR 0014's,
ADR 0020's superseded text, the two `docs/superpowers/specs/` heterogeneous-OS
design specs, every RELEASED-section `CHANGELOG.md` entry, and `VERSIONS.md`'s
v0.10.1 / v0.12.0 release rows all stand as written. They were true when
written and remain true as history; a later reader must not "fix" them.

**The cost, stated plainly.** alp-sdk loses its pre-silicon simulator coverage
outright. There is no replacement and none is planned: after this change no CI
job boots any alp-sdk image at all, on any core, so a regression that breaks
the AEN M55 or the V2N M33 boot path is caught on the bench or not caught.
`pr-twister-aen.yml` and `pr-getting-started-aen801.yml` become the only CI
that cross-compiles for the AEN801 boards, and both are compile-only — they
prove the tree still builds for `alp_e1m_aen801_m55_he` / `_hp`, never that the
result runs. The V2N M33 sci0 console path loses its only automated check
entirely. Amendment 1's "retiring the verb costs alp-sdk no simulator coverage"
was accurate about the verb alone; widening to the CI is precisely what makes
it false. That loss is accepted with open eyes, not mitigated.

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
