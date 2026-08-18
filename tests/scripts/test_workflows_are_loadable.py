# SPDX-License-Identifier: Apache-2.0
"""Every `.github/workflows/*.yml` must actually LOAD.

Two workflows shipped on `dev` that GitHub Actions could not parse, and
nothing noticed for hours:

  * `pr-doc-drift.yml` carried nine unresolved merge-conflict marker lines
    (`<<<<<<< HEAD`, `=======`, `>>>>>>> 8e72b3dc ...`) straight into a
    merge commit.
  * `pr-metadata-validate.yml:336` had an unquoted plain scalar containing
    a colon-space -- `- name: Gate — Zephyr core app: <-> CMakeLists.txt
    --core mapping` -- which is a YAML mapping-value error, not a style nit.

A workflow that fails to load does not fail loudly. It reports as a
`failure` whose duration is **0s**, and because neither of these is a
required status context, nothing blocked on it: roughly thirty metadata
gates and every doc-drift gate silently stopped running while PRs kept
merging green.

`bash -n`, shellcheck and the pytest sweep all pass over a repo in that
state -- none of them parse workflow YAML. Hence this test.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO / ".github" / "workflows"

# Anchored at line start so prose that merely mentions a marker (a comment
# explaining this very failure, for instance) is not a false positive.
_CONFLICT_MARKER = re.compile(r"^(?:<{7} |={7}$|>{7} )", re.M)


def _workflow_files() -> list[Path]:
    return sorted([*WORKFLOWS.glob("*.yml"), *WORKFLOWS.glob("*.yaml")])


def test_there_are_workflows_to_check() -> None:
    """Guard against the guard: if the glob ever returns nothing, every
    parametrised test below would vanish and the suite would still be
    green while covering nothing."""
    found = _workflow_files()
    assert len(found) >= 20, f"expected >=20 workflow files, found {len(found)} -- glob has drifted"


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_parses_as_yaml(wf: Path) -> None:
    """The check that would have caught pr-metadata-validate.yml:336.

    A step name containing `: ` must be quoted. Unquoted, YAML reads it as
    a nested mapping key and the whole document is invalid.
    """
    text = wf.read_text(encoding="utf-8")
    try:
        doc = yaml.safe_load(text)
    except yaml.YAMLError as exc:  # pragma: no cover - the failure path IS the test
        pytest.fail(
            f"{wf.name} is not loadable YAML -- GitHub Actions will report this workflow as a "
            f"0-second 'failure' and every gate inside it silently stops running:\n{exc}"
        )
    assert isinstance(doc, dict), f"{wf.name} did not parse to a mapping (got {type(doc).__name__})"
    assert "jobs" in doc, f"{wf.name} parsed but declares no `jobs:`"


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_has_no_conflict_markers(wf: Path) -> None:
    """The check that would have caught pr-doc-drift.yml.

    Kept separate from the parse test on purpose: a conflict-marked file
    usually also fails to parse, but not always -- markers landing inside a
    block scalar or a comment-heavy region can still load, and would then
    ship semantically wrong triggers rather than an obvious error.
    """
    hits = [
        f"{wf.name}:{text[: m.start()].count(chr(10)) + 1}"
        for text in [wf.read_text(encoding="utf-8")]
        for m in _CONFLICT_MARKER.finditer(text)
    ]
    assert not hits, "unresolved merge-conflict markers committed in a workflow: " + ", ".join(hits)


# Contexts whose value is chosen by somebody other than this repo's
# workflow authors: a PR/issue/comment/review author, their own GitHub
# login, whoever named the branch or tag a run fires on, whoever typed a
# `workflow_dispatch` input, and whoever wrote the commit messages a push
# carries. See docs/ci/runner-architecture.md's
# "Untrusted-input handling in run: blocks" section for the rule this test
# enforces (alp-sdk#1475): `pr-bitbake.yml` spliced
# `${{ github.event.pull_request.head.ref }}` -- a fork PR's own branch
# name, which may legally contain shell metacharacters ($(), backticks, ;,
# &, | are all valid in git-check-ref-format -- directly into a `run:`
# block's SOURCE TEXT. GitHub substitutes `${{ }}` into that text *before*
# bash ever parses it, so this is a template injection, not a quoting bug --
# quoting inside the script does not help, and the fix is always to route
# the value through the step's own `env:` block and reference a quoted
# shell variable instead.
#
# Matching is plain substring containment, so `github.ref` also covers
# `github.ref_name`, `github.ref_type` and `github.ref_protected`;
# `github.ref_name` is listed anyway because it is the form the fixed
# `dispatch-tan-parity.yml` site used and the one a reader looks for.
_ATTACKER_CONTEXTS = (
    "github.event.pull_request",
    "github.event.issue",
    "github.event.comment",
    "github.event.review",
    "github.event.head_commit",
    "github.event.commits",
    "github.event.inputs",
    "github.event.workflow_run",
    "github.head_ref",
    "github.ref_name",
    "github.ref",
    "github.actor",
)

# One `${{ ... }}` expression at a time, non-greedy so two expressions on
# the same line are matched separately rather than merged into one span
# running from the first `${{` to the last `}}`.
_EXPRESSION = re.compile(r"\$\{\{.*?\}\}")


# The two sinks where GitHub substitutes a `${{ }}` expression into text an
# interpreter then parses as SOURCE. Keyed by the remedy, because the fix
# differs: a shell body reads the value back as `"$VAR"`, a github-script
# body reads it as `process.env.VAR`.
_RUN_SINK = "a `run:` body"
_SCRIPT_SINK = "an `actions/github-script` `with: script:` body"

_REMEDY = {
    _RUN_SINK: "route it through that step's env: block instead and reference a quoted shell "
    "variable (e.g. env: {VAR: %s}, run: echo \"$VAR\")",
    _SCRIPT_SINK: "route it through that step's env: block instead and read it back with "
    "process.env (e.g. env: {VAR: %s}, script: core.info(process.env.VAR))",
}


def _source_text_steps(doc: object) -> list[tuple[str, str, str, str]]:
    """(job_id, step_name, sink, text) for every step in `doc` that hands
    GitHub-substituted text to an interpreter: a `run:` body (shell) and an
    `actions/github-script` `with: script:` body (JavaScript). A step with
    neither -- an ordinary `uses:` action, for instance -- contributes no
    source text and is out of scope for this check.

    `script:` is checked only when the step's `uses:` names
    `actions/github-script`; another action's `with: script:` input is just
    a string it receives, not something it evals.
    """
    out: list[tuple[str, str, str, str]] = []
    if not isinstance(doc, dict):
        return out
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return out
    for job_id, job in jobs.items():
        if not isinstance(job, dict):
            continue
        steps = job.get("steps")
        if not isinstance(steps, list):
            continue
        for idx, step in enumerate(steps):
            if not isinstance(step, dict):
                continue
            step_name = str(step.get("name", f"step[{idx}]"))
            run_text = step.get("run")
            if isinstance(run_text, str):
                out.append((str(job_id), step_name, _RUN_SINK, run_text))
            uses = step.get("uses")
            with_block = step.get("with")
            if isinstance(uses, str) and uses.startswith("actions/github-script") and isinstance(with_block, dict):
                script_text = with_block.get("script")
                if isinstance(script_text, str):
                    out.append((str(job_id), step_name, _SCRIPT_SINK, script_text))
    return out


def _attacker_context_splices(source_text: str) -> list[str]:
    """Every `${{ ... }}` expression in `source_text` that references one of
    `_ATTACKER_CONTEXTS` directly, in the order it appears."""
    return [expr for expr in _EXPRESSION.findall(source_text) if any(ctx in expr for ctx in _ATTACKER_CONTEXTS)]


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_run_blocks_do_not_splice_attacker_context(wf: Path) -> None:
    """The check that would have caught #1475. Until now the invariant
    "route every `_ATTACKER_CONTEXTS` member through env:, never splice it
    into a run: block" was defended only by a comment at each fixed site --
    nothing failed CI if a future edit reintroduced the construct, here or
    in a workflow file nobody thought to re-sweep.

    SCOPE: both text sinks GitHub substitutes into before an interpreter
    parses -- a step's `run:` body, and an `actions/github-script`
    `with: script:` body (alp-sdk#1529; the `script:` half shipped covered
    but unexercised, the repo's one github-script step interpolates
    nothing). What is still NOT covered is a value arriving transitively
    through `steps.<id>.outputs.*`: matching is direct substring
    containment, so a green run here is not "no template-injection sink
    anywhere".

    PROOF this is a real regression test, not a tautology: run against the
    `.github/workflows/` tree of alp-sdk@63a5eceb (the commit #1475 was
    filed against, before any of its fix landed), this parametrised test
    fails on exactly three files -- `pr-bitbake.yml`,
    `dispatch-tan-parity.yml` and `pr-static-analysis.yml`, i.e. all three
    sites the fix touches -- and passes on the other 26. With the shorter
    `_ATTACKER_CONTEXTS` this test first shipped with, the
    `dispatch-tan-parity.yml` node PASSED against that same unfixed tree:
    its splice is `${{ github.ref_name }}`, which matched no member of the
    list, so the gate failed open on a site this very commit fixes.
    """
    doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
    violations = [
        f"{wf.name}: job `{job_id}` step `{step_name}` splices {expr!r} directly into "
        f"{sink}'s source text -- " + (_REMEDY[sink] % expr) + "; see "
        "docs/ci/runner-architecture.md's \"Untrusted-input handling in run: blocks\""
        for job_id, step_name, sink, source_text in _source_text_steps(doc)
        for expr in _attacker_context_splices(source_text)
    ]
    assert not violations, "\n".join(violations)


def test_attacker_context_splice_detector_catches_a_seeded_violation() -> None:
    """Guard against the guard: if `_attacker_context_splices` ever stopped
    matching anything -- an over-narrowed regex, a refactor that stops
    walking `steps:` correctly -- the test above would report green over a
    workflow tree that still splices attacker context into a run: block,
    silently, the same way the original #1475 defect sat unnoticed as a
    YAML comment for hours. Prove the detector fires on the exact
    construct it exists to catch, and does NOT fire on the safe patterns
    this repo's workflows actually use (env:-block assignment, a step
    output derived from repo-controlled data, a GitHub-assigned SHA),
    before trusting it against the real tree.
    """
    seeded_violation = 'echo "${{ github.event.pull_request.head.ref }}"\n'
    assert _attacker_context_splices(seeded_violation) == ["${{ github.event.pull_request.head.ref }}"]

    seeded_actor = 'echo "${{ github.actor }} did it"\n'
    assert _attacker_context_splices(seeded_actor) == ["${{ github.actor }}"]

    for safe_run_text in (
        'echo "${{ github.sha }}"\n',
        'echo "${{ steps.t.outputs.sha }}"\n',
        'echo "${{ matrix.som }}"\n',
        'echo "${{ needs.build.outputs.tag }}"\n',
        # env:-block indirection itself is not a run: string at all -- this
        # line only proves the detector does not choke on the safe shell
        # variable form the fix produces.
        'echo "$HEAD_REF"\n',
    ):
        assert _attacker_context_splices(safe_run_text) == [], safe_run_text

    # The env:-block form is safe because it is a mapping VALUE, not source
    # text -- _source_text_steps() only ever yields a step's `run` body and
    # a github-script `script:` body, so an env: assignment of the same
    # expression must never be reported.
    doc_with_env_indirection = {
        "jobs": {
            "dispatch": {
                "steps": [
                    {
                        "name": "Resolve",
                        "env": {"HEAD_REF": "${{ github.event.pull_request.head.ref }}"},
                        "run": 'echo "ref=$HEAD_REF" >> "$GITHUB_OUTPUT"\n',
                    }
                ]
            }
        }
    }
    assert _source_text_steps(doc_with_env_indirection) == [
        ("dispatch", "Resolve", _RUN_SINK, 'echo "ref=$HEAD_REF" >> "$GITHUB_OUTPUT"\n')
    ]
    for _, _, _, source_text in _source_text_steps(doc_with_env_indirection):
        assert _attacker_context_splices(source_text) == []


def test_github_script_bodies_are_walked_as_a_second_sink() -> None:
    """Guard against the guard, for the sink #1529 added. The repo's one
    `actions/github-script` step (`pr-abi-snapshot.yml`) interpolates no
    `${{ }}` at all, so the parametrised test above exercises this branch
    with zero inputs -- it would stay green if `_source_text_steps` stopped
    yielding `script:` bodies entirely. Seed the construct instead.

    Also pins the deliberate narrowness: a `with: script:` on some OTHER
    action is an input string that action receives, not JavaScript it
    evals, so reporting it would be a false positive.
    """
    doc = {
        "jobs": {
            "comment": {
                "steps": [
                    {
                        "name": "Post",
                        "uses": "actions/github-script@v9",
                        "with": {"script": 'core.info("${{ github.event.pull_request.head.ref }}")\n'},
                    },
                    {
                        "name": "Not an evaluator",
                        "uses": "some/other-action@v1",
                        "with": {"script": 'echo "${{ github.event.pull_request.title }}"\n'},
                    },
                ]
            }
        }
    }
    assert _source_text_steps(doc) == [
        ("comment", "Post", _SCRIPT_SINK, 'core.info("${{ github.event.pull_request.head.ref }}")\n')
    ]
    for _, _, _, source_text in _source_text_steps(doc):
        assert _attacker_context_splices(source_text) == ["${{ github.event.pull_request.head.ref }}"]


# alp-sdk#1479: a mutable `@<tag>` reference on a third-party `uses:` means
# a retagged or compromised upstream release changes what a workflow
# executes on its next run, with no new PR to review. `_SHA_PIN` is what a
# fixed reference must look like: the 40 hex characters of a full commit
# SHA, nothing shorter (a 7-char abbreviation is still spoofable) and no
# branch/tag name.
_SHA_PIN = re.compile(r"^[0-9a-f]{40}$")

# The ONE reference in this repo deliberately left tag-pinned. Upstream's
# own README ("Referencing SLSA builders and generators") requires its
# builders/generators be referenced as `@vX.Y.Z` -- not a shorter `@vX.Y`
# or `@vX`, which upstream's README says will fail the build -- so
# slsa-verifier can verify the ref of the trusted reusable workflow; a hash
# pin is not supported (tracked upstream as slsa-verifier#12). See the
# comment above `release.yml`'s `provenance:` job and
# docs/ci/runner-architecture.md's "Third-party action pinning" section,
# which this test's docstring and this comment must stay in sync with.
# `_EXEMPT_TAG_PIN` enforces the full three-part form: a bare prefix match
# with no assertion on the ref would let a future bump to `@v2` or
# `@v2.2` pass this gate silently -- the same blind spot #1479 fixed for
# everything else.
_TAG_PIN_EXEMPT_PREFIX = "slsa-framework/slsa-github-generator/"
_EXEMPT_TAG_PIN = re.compile(r"^v\d+\.\d+\.\d+$")


def _uses_values(doc: object) -> list[tuple[str, str, str]]:
    """(job_id, step_name, uses_value) for every `uses:` in `doc` -- a
    step's own `uses:`, and a job's top-level `uses:` for a
    reusable-workflow-call job (no `runs-on:`, e.g. `provenance` in
    release.yml)."""
    out: list[tuple[str, str, str]] = []
    if not isinstance(doc, dict):
        return out
    jobs = doc.get("jobs")
    if not isinstance(jobs, dict):
        return out
    for job_id, job in jobs.items():
        if not isinstance(job, dict):
            continue
        job_uses = job.get("uses")
        if isinstance(job_uses, str):
            out.append((str(job_id), "(reusable-workflow job)", job_uses))
        steps = job.get("steps")
        if not isinstance(steps, list):
            continue
        for idx, step in enumerate(steps):
            if not isinstance(step, dict):
                continue
            step_name = str(step.get("name", f"step[{idx}]"))
            uses = step.get("uses")
            if isinstance(uses, str):
                out.append((str(job_id), step_name, uses))
    return out


def _uses_violation(wf_name: str, job_id: str, step_name: str, uses: str) -> str | None:
    """Return a violation message if `uses` isn't correctly pinned, else
    None. Split out of `test_workflow_uses_are_sha_pinned` so the exempt
    branch's own ref format can be unit-tested directly, without needing a
    seeded workflow file on disk."""
    if uses.startswith("./") or uses.startswith("docker://"):
        return None
    if uses.startswith(_TAG_PIN_EXEMPT_PREFIX):
        ref = uses.rsplit("@", 1)[-1] if "@" in uses else ""
        if _EXEMPT_TAG_PIN.match(ref):
            return None
        return (
            f"{wf_name}: job `{job_id}` step `{step_name}` uses `{uses}` -- the documented "
            "slsa-github-generator exemption only covers a full `vX.Y.Z` ref, not a shorter "
            "`vX.Y`/`vX` or a non-tag ref (alp-sdk#1479); see docs/ci/runner-architecture.md's "
            '"Third-party action pinning"'
        )
    ref = uses.rsplit("@", 1)[-1] if "@" in uses else ""
    if not _SHA_PIN.match(ref):
        return (
            f"{wf_name}: job `{job_id}` step `{step_name}` uses `{uses}` -- not pinned to a "
            "40-character commit SHA (alp-sdk#1479); see docs/ci/runner-architecture.md's "
            '"Third-party action pinning"'
        )
    return None


@pytest.mark.parametrize("wf", _workflow_files(), ids=lambda p: p.name)
def test_workflow_uses_are_sha_pinned(wf: Path) -> None:
    """The check #1479's own follow-on left undone: without this, the next
    PR that adds a `uses: some/action@v1` step silently reintroduces a
    mutable-tag pin and nothing catches it -- the sweep #1479 did was a
    one-shot, not an invariant, until this test exists.

    A local composite action (`uses: ./...`) is out of scope: there is no
    upstream tag to pin, it is this repo's own code at whatever commit
    checked it out. `slsa-framework/slsa-github-generator`'s
    reusable-workflow call is the one documented exception (see
    `_TAG_PIN_EXEMPT_PREFIX`), and even that exemption requires a full
    `vX.Y.Z` ref (see `_EXEMPT_TAG_PIN` and
    `test_exempt_slsa_ref_must_be_full_semver_tag`) -- a bare prefix match
    with no assertion on the ref would let a future `@v2` or `@v2.2` bump
    slip past this gate exactly like the SHA-pin blocker it exists to catch.

    PROOF this is a real regression test: run against
    `.github/workflows/release.yml` from alp-sdk@03f44889 (the commit
    #1479 branched from, before any of its fix landed), this test fails on
    3 of that file's 4 `uses:` lines -- `actions/checkout@v6`,
    `actions/setup-python@v6` and `softprops/action-gh-release@v3`, each
    still resolving a bare tag; the 4th, the SLSA generator call, is
    already `@v2.1.0` and correctly exempt either way.
    """
    doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
    violations = []
    for job_id, step_name, uses in _uses_values(doc):
        violation = _uses_violation(wf.name, job_id, step_name, uses)
        if violation:
            violations.append(violation)
    assert not violations, "\n".join(violations)


def test_sha_pin_detector_catches_a_seeded_tag_reference() -> None:
    """Guard against the guard: if `_uses_values` ever stopped walking
    steps correctly, or `_SHA_PIN` were loosened, the test above would
    report green over a workflow that still resolves a mutable tag."""
    seeded = {
        "jobs": {
            "build": {
                "steps": [
                    {"name": "Checkout", "uses": "actions/checkout@v4"},
                    {
                        "name": "Pinned",
                        "uses": "actions/setup-python@a26af69be951a213d495a4c3e4e4022e16d87065",
                    },
                    {"name": "Local", "uses": "./.github/actions/thing"},
                ]
            },
            "provenance": {
                "uses": "slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@v2.1.0",
            },
        }
    }
    values = _uses_values(seeded)
    assert ("build", "Checkout", "actions/checkout@v4") in values
    assert not _SHA_PIN.match("v4")
    assert _SHA_PIN.match("a26af69be951a213d495a4c3e4e4022e16d87065")

    # End-to-end: a broad early-return added to `_uses_violation` (e.g. one
    # that short-circuits before the `_SHA_PIN` check) would leave the
    # assertions above green while the real gate passes over a mutable-tag
    # workflow. Call `_uses_violation` itself on the seeded data so that
    # class of regression fails here too.
    assert _uses_violation("seeded.yml", "build", "Checkout", "actions/checkout@v4") is not None
    assert (
        _uses_violation(
            "seeded.yml",
            "build",
            "Pinned",
            "actions/setup-python@a26af69be951a213d495a4c3e4e4022e16d87065",
        )
        is None
    )


def test_exempt_slsa_ref_must_be_full_semver_tag() -> None:
    """Fix-round follow-up to #1479: the exempt branch used to be a bare
    `uses.startswith(_TAG_PIN_EXEMPT_PREFIX)` with no check on the ref
    itself, so ANY ref on that prefix -- including a future `@v2` or
    `@v2.2` bump -- passed the gate silently, the same blind spot the SHA
    pin closed for every other action. This asserts `_uses_violation`
    rejects anything but a full `vX.Y.Z` ref on the exempt prefix."""
    exempt_prefix = "slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml"
    assert _uses_violation("release.yml", "provenance", "(reusable-workflow job)", f"{exempt_prefix}@v2.1.0") is None
    for bad_ref in ("v2", "v2.1", "main", "a26af69be951a213d495a4c3e4e4022e16d87065"):
        assert (
            _uses_violation("release.yml", "provenance", "(reusable-workflow job)", f"{exempt_prefix}@{bad_ref}")
            is not None
        )
