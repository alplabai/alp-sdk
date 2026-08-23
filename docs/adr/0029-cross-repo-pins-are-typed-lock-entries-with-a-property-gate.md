# 0029. Cross-repo pins are typed lock-manifest entries, verified by a property gate — not prose intent and spelling checks

Status: Proposed
Date: 2026-08-16

## Context

Nine failures over the last 48 hours, all real, all pin-related:

1. `tools/native-sim-container/Containerfile`'s `ARG ZEPHYR_REV=v4.4.0` drifted
   from `west.yml`'s `v4.4.1`; no gate covered it (alp-sdk#1458). Fixed by
   deriving from `west.yml`.
2. The first attempt at that fix re-created the duplication in the file's own
   header comment, and `--fix` propagated to one copy and not the other, then
   reported "all agree".
3. `docs/zephyr-version-policy.md`'s hand-maintained "every pin site" table
   went stale the moment a pin site was added; nothing gates the enumeration.
4. alp-sdk-vscode's `SUPPORTED_CLI_VERSION = "0.5.1"` named a published tan
   release that could not build ANY Renesas (tan-cli#639) or Alif (tan-cli#728)
   SoM. `check-cli-pin.mjs` passed throughout — it verifies the pin names a
   *published* release.
5. tan-cli's `test_strict_loaders_matches_its_pinned_sdk_source` compares a
   pinned hash against a CI checkout pinned to the very commit the hash came
   from — it can catch a typo, never real drift — and unlike its two siblings
   had no live-drift alarm (tan-cli#761, since fixed).
6. `STRICT_LOADERS_PINNED_SDK_COMMIT` is deliberately frozen at the commit
   that INTRODUCED a known gap while its two siblings track `dev`. Nothing in
   the code distinguishes those intents, and a wrong bug report (tan-cli#755)
   was filed because of it.
7. tan-cli#766 had to pin one commit SHORT of an upstream release bump: the
   newer commit makes the scaffold emit render doc links to a `v0.16.0` tag
   that does not exist, so re-vendoring would ship 404s to every customer who
   scaffolds. "Always pin latest" is therefore wrong.
8. tan-cli `dev` carried a version equal to an already-published tag, failing
   a gate on every PR until bumped (tan-cli#764).
9. alp-sdk's release back-merge is squashed by the merge queue, so `main`'s
   history is not an ancestor of `dev`, making "is this commit on dev?"
   subtle enough that a pin comment got it wrong.

Reading these together, four distinct things are currently handled by one
undifferentiated mechanism — a literal value plus a prose comment explaining
it:

- **Fork-audit bookkeeping** — tan-cli's three pins (`STRICT_LOADERS_PINNED_SDK_COMMIT`
  and its two siblings) plus ~40 hashes, recording when a human last diffed
  upstream against a hand-maintained fork.
- **Propagated constant** — alp-sdk's Zephyr version, which must read the same
  across 8+ sites (`west.yml`, the container `ARG`, its own header comment,
  the policy doc's table, …).
- **Compatibility declaration** — `SUPPORTED_CLI_VERSION`, alp-sdk-vscode's
  claim that a named tan release can build the SoMs it ships against.
- **Spelling agreement** — `PINNED_SDK_TAG` in tan-cli matching the literal
  `ref:` string in a workflow YAML; two copies of the same token that must
  read identically, nothing more.

Each failure above is exactly one of these four models being verified as if
it were a different one: #1–#3 are a propagated constant with no
enumeration gate; #4 is a compatibility declaration verified as a spelling
agreement (the pin's *name* was checked, never whether the named build
works); #5–#6 are fork-audit bookkeeping with the tracking/frozen distinction
erased, so a deliberately-frozen entry reads as a stale one; #7 shows a
tracking pin cannot simply chase the newest upstream value, because "newest"
can be broken for the consumer even when it is valid upstream; #8 is a
propagated-constant collision (two different producers landing on the same
value) with no cross-repo visibility; #9 is the same mechanism applied to
ancestry — a hand-written claim about repo history instead of a computed one.

## Decision

**Pinning itself is correct and is not what changes.** Observation 7 proves
"always pin latest" is wrong for a tracking pin with a broken upstream head,
and reproducibility across independent release cadences rules out "no pin".
What changes is representation and acceptance.

### 1. Representation: a machine-readable lock manifest

Every cross-repo pin becomes an entry in a lock manifest, not a literal
value plus a prose comment. Each entry carries:

- `value` — the pinned literal (a SHA, a tag, a version string, a hash).
- `intent: tracking | frozen` — `tracking` entries are expected to move as
  their target moves (e.g. "the tag of the latest published tan-cli
  release"); `frozen` entries are pinned on purpose and do not move on
  their own.
- a **reason-issue link**, required on every `frozen` entry — the issue that
  explains *why* this entry stopped tracking (tan-cli#766's "one commit
  short of the `v0.16.0` doc-link break" is exactly the text this field
  exists to hold). A `frozen` entry with no reason-issue link is a schema
  violation, not a documentation gap.
- the **tracking target**, on every `tracking` entry — what it tracks (a
  branch, a release channel, "latest tag") so a checker can compute
  "is this pin behind its target" without guessing from the value's shape.
- a **proof reference** — the run/attestation that last certified this
  value works (see the property gate, below). A pin with no proof reference
  is unverified by definition, not merely undocumented.
- a **site list**, for values that must exist as literal copies because the
  consumer cannot read the manifest at that point (a Dockerfile `ARG`, a
  workflow YAML `ref:` string). This is what turns "every pin site" from
  prose (docs/zephyr-version-policy.md's table, which went stale the moment
  a site was added — observation 3) into a generated, checkable list.

Intent stops living in prose. A reviewer — or a script — reads `intent:
frozen` and the reason-issue link instead of inferring from a comment
whether a pin is supposed to move (observation 6's exact failure).

### 2. Acceptance semantics: verify the property, not the spelling

A gate that accepts a pin move must verify the thing the pin *certifies*,
not merely that two copies of a string agree. Concretely: **no pin entry
moves without a green run on the new value.** `check-cli-pin.mjs` passing
throughout observation 4 is the proof this matters — it confirmed the pin
*named* a published release; it never asked whether that release could
build a Renesas or an Alif SoM. The property `SUPPORTED_CLI_VERSION`
actually claims — "this tan version can build these SoMs" — was never
checked by anything.

### 3. Enforcement, in three layers

- **Derive wherever a consumer can read the manifest.** Where a build step,
  a script, or a CI job can load the lock manifest directly, it does — no
  second copy to drift.
- **A generated site list plus a value sweep**, for the sites that cannot
  derive. The site list is generated from the manifest's `site list` field
  (fixing observation 3's stale enumeration); the value sweep then greps
  every file class the manifest declares pin-bearing and diffs what it
  finds against the manifest, so an *unlisted* site — a new pin site nobody
  added to the manifest — is caught by the sweep finding a value the
  manifest doesn't know about, not silently missed the way #1's Containerfile
  `ARG` was.
- **The property gate on movement** — clause 2, restated as an enforcement
  layer: a manifest edit that changes a `value` is rejected unless it
  carries a proof reference to a green run on that value. This is what
  fixes observation 4: a `SUPPORTED_CLI_VERSION` bump would require the new
  tan version to have actually built the SoMs it claims to support.

### 4. Ancestry is computed, never stated

Observation 9 is a narrower case of the same defect: a hand-written claim
("this commit is on `dev`") standing in for a fact a machine can compute.
**Never state ancestry in a comment.** If a checker's correctness depends on
one ref being an ancestor of another, it calls `git merge-base
--is-ancestor` at gate time. Because alp-sdk's release back-merge is
squashed by the merge queue, `main`'s history is not literally an ancestor
of `dev` — a squash-rewritten history makes that check compute **false**,
which must fail the gate loudly (a red check) rather than let a comment
assert something the actual graph no longer supports.

### 5. The contract this ADR fixes, verbatim

The property gate (clause 2/3) needs somewhere to run the "does this
combination actually build" check across repos. This is the cross-repo
contract two repos — alp-sdk and tan-cli — will implement:

- Event: `repository_dispatch`, type `pin-move-verify`, sent to
  `alplabai/alp-e2e`
- `client_payload`:
  ```json
  {
    "tan_ref": "<str>",
    "sdk_ref": "<str>",
    "soms": ["<SKU>", "..."],
    "source_repo": "<owner/name>",
    "source_sha": "<str>",
    "source_pr": "<int|null>"
  }
  ```
- alp-e2e reports a **Check Run** back on `source_repo` at `source_sha`,
  named `pin-verify · <tan_ref> × <sdk_ref>`
- alp-e2e emits an **attestation**:
  ```json
  {
    "tan_ref": "...",
    "sdk_ref": "...",
    "soms": ["..."],
    "result": "...",
    "run_url": "...",
    "timestamp": "...",
    "artefacts": ["..."]
  }
  ```
- Auth: the existing GitHub App pattern from
  `.github/workflows/dispatch-tan-parity.yml`
  (`actions/create-github-app-token`, `ALP_CI_APP_ID`), scoped to the target —
  a short-lived token minted per run, no long-lived PAT, no secret exposed
  to fork PRs.

The `proof reference` field in clause 1 is this contract's attestation
(`run_url` + `artefacts`), and a manifest checker validates a moved `value`
against it rather than trusting an adjacent comment.

### Sequencing

1. The pin-move e2e gate (clause 5's contract) — changes acceptance
   semantics without moving any constant.
2. The lock manifest, in tan-cli only.
3. alp-sdk's Zephyr-version domain: the value sweep (clause 3) plus
   generating `docs/zephyr-version-policy.md`'s table from the checker's
   registry instead of hand-maintaining it.
4. An attestation-aware `check-cli-pin.mjs`, consuming clause 5's
   attestations instead of only checking spelling.
5. The fork-surface decision — separately (see Consequences).

## Alternatives

- **"The model is fine, tool it better."** Loses on observations 4 and 6.
  `check-cli-pin.mjs` was working exactly as designed; the defect was that
  design's target (spelling, not the property the pin certifies). Better
  tooling cannot enforce a `tracking`/`frozen` distinction the representation
  itself does not carry — #6's wrong bug report happened because nothing in
  the code, however carefully tooled, could tell a deliberately-frozen pin
  from a stale one.
- **Full derivation everywhere; no manifest, no literal copies.** Loses
  because some sites genuinely cannot derive (a Dockerfile `ARG`, a
  third-party tool's config format that only accepts a literal), and
  observations 5–7 show that different pins legitimately hold **different**
  values by intent — a tracking pin and a frozen pin sitting at different
  commits is not drift to eliminate, it is the correct state. A
  derive-everywhere model has no way to express clause 1's `intent` field at
  all.
- **A monorepo**, merging tan-cli and alp-sdk (and by extension
  alp-sdk-vscode) into one repository. This would dissolve the tan↔sdk pin
  class outright — there would be nothing to pin. It loses **today** on
  independent release cadences, the public/private split, and marketplace
  publishing (tan-cli and alp-sdk-vscode ship through different channels
  with different audiences and licensing). Recorded here honestly because it
  is the alternative that would change the answer if the pin count keeps
  growing — not adopted now, but not dismissed as never-worth-revisiting.

## Consequences

**Good.** Each of the four models (fork-audit bookkeeping, propagated
constant, compatibility declaration, spelling agreement) gets a
representation that can say what kind of pin it is and what verifies it.
Observations 1, 3, 4, 6, and 9 each map to a specific clause above that
closes them structurally rather than by adding one more ad hoc check.

**Bad / costs, stated honestly, not glossed over:**

- **The manifest is itself a meta-layer and can rot exactly as
  `docs/zephyr-version-policy.md`'s table did.** A lock manifest nobody
  re-checks against reality degrades the same way a hand-maintained table
  does. Two mitigations are load-bearing, not optional: the value sweep
  (clause 3) checks the manifest **against reality**, not only reality
  against the manifest, so a site drifting out from under the manifest is
  caught the same way a site missing from the manifest is; and a scheduled
  run on the HELD tuple (a `frozen` pin's current value) needs no human
  memory to trigger it — a pin can rot while sitting perfectly still,
  because the code it points at can still break against a moving world
  around it.
- **The pin-move e2e gate (clause 5) proves compile/link fitness in roughly
  five minutes** — exactly the class of break `SUPPORTED_CLI_VERSION =
  "0.5.1"` shipped despite (observation 4) — **but a runtime-broken tuple
  still passes it.** It complements the existing parity oracle and periodic
  HIL runs; it does not replace either. "Green e2e" must not become the new
  "check-cli-pin passed throughout" — a fast, narrow gate treated as a full
  guarantee is the same mistake this ADR is fixing, one layer up.
- **tan-cli's pin apparatus is fork-audit bookkeeping, and its cost is
  proportional to the FORKED SURFACE, not to pin tooling.** No pin
  representation shrinks the ~40 hashes or the audit burden they represent —
  that surface exists because `python/tan/planner/` hand-ports code out of
  `scripts/alp_orchestrate`. Shrinking it is a separate decision about
  package-izing `scripts/alp_orchestrate` so tan can depend on it instead of
  forking it, and this ADR deliberately does not attempt that decision —
  see Sequencing step 5.
- **Live illustration of the exact defect class this ADR addresses:** ADR
  numbers are themselves a hand-allocated identifier with no gate. #1470 and
  this ADR both initially targeted `0028` — a monotonic counter maintained
  only in reviewers' heads, caught here only because a human checked the open
  PR list before writing this file. Same class of failure as observations
  1–9; not fixed by this ADR, recorded so it is not lost.

## Migration

None — this ADR records the decision and the contract shape. Implementation
follows the Sequencing list above, each step its own PR, none of which this
ADR itself performs.
