# SPDX-License-Identifier: Apache-2.0
"""`metadata/model_perf/**` -- bench-measured model perf points (tier 2).

A perf point is the one data asset in this tree a customer reads as an EXACT
answer about their own hardware, on our authority, with no NPU toolchain and
no board of their own to check it against.  So every check here exists to stop
a point quietly ceasing to describe the run that produced it.

THE VACUOUS-PASS TRAP, and how it is closed.  `metadata/model_perf/` is empty
until the first bench campaign, so a glob over it matches nothing and every
`for point in points: assert ...` below passes over an empty set -- green for
the wrong reason, which is precisely the shape of gate this programme has
already shipped five times.  Two things close it:

  * `tests/fixtures/model_perf/` carries a real, complete perf point that
    every directory-wide check runs against, and
    `test_the_perf_point_checks_are_not_vacuous` fails if that fixture ever
    disappears.
  * `test_each_rule_bites`, parametrized over `_MUTATIONS`, drives the SHIPPED
    gate -- the real `metadata/schemas/model-perf-v1.schema.json` plus the real
    `validate_metadata._check_model_perf_semantics`, not a re-implementation
    -- over a mutation of every single rule, asserting the unmutated document
    passes and each mutant fails.  A rule that stops biting fails a test here
    rather than going quietly green.

The fixture is marked `_fixture`, and the gate REFUSES that key under
`metadata/model_perf/`, so its placeholder figures cannot be promoted into the
published tree and read as bench data.
"""
from __future__ import annotations

import copy
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import jsonschema
import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMA_PATH = _ROOT / "metadata" / "schemas" / "model-perf-v1.schema.json"
_PUBLISHED = _ROOT / "metadata" / "model_perf"
_FIXTURES = _ROOT / "tests" / "fixtures" / "model_perf"

sys.path.insert(0, str(_ROOT / "scripts"))
import validate_metadata as V  # noqa: E402


def _points(root: Path) -> list[Path]:
    """`**`, not `*/*/*.json`: a file dropped at the wrong depth must still be
    picked up and rejected by the path-identity check, never silently matched
    by nothing."""
    return sorted(root.glob("**/*.json")) if root.is_dir() else []


#: Published points (none yet) AND the fixture, so every directory-wide check
#: below runs against at least one real document today and inherits the first
#: campaign's output automatically.
_ALL_POINTS = _points(_PUBLISHED) + _points(_FIXTURES)

#: The canonical fixture, pinned by path rather than picked off the glob: the
#: mutation suite derives every mutant from it, so it has to be THIS document
#: and not "whichever fixture sorts first".
_STEM = "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+1e562a678c9f"
_BASE = _FIXTURES / "E1M-AEN801" / "ethos-u85-256" / f"{_STEM}.json"
_BASE_REL = f"tests/fixtures/model_perf/E1M-AEN801/ethos-u85-256/{_STEM}.json"

#: Where the same document would live if it were a real, published point.
#: Used to prove the published shape passes AND that `_fixture` is refused
#: there.
_PUBLISHED_REL = f"metadata/model_perf/E1M-AEN801/ethos-u85-256/{_STEM}.json"


def _load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _ids(path: Path) -> str:
    return path.relative_to(_ROOT).as_posix()


# ---------------------------------------------------------------------------
# The non-vacuity guard itself.
# ---------------------------------------------------------------------------

def test_the_perf_point_checks_are_not_vacuous():
    """Every parametrized check in this file globs a directory that is legally
    allowed to be empty.  Without a fixture on disk they would all pass while
    checking nothing, which is indistinguishable from a suite that works."""
    assert _points(_FIXTURES), (
        "tests/fixtures/model_perf/ carries no perf point, so every "
        "directory-wide check in this file now passes over an empty set")
    assert _BASE.is_file(), f"the pinned mutation base {_BASE_REL} is gone"


# ---------------------------------------------------------------------------
# Directory-wide checks -- parametrized over the glob, so a point added by the
# first bench campaign inherits all of them with no list to hand-update.
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_every_perf_point_pins_the_model_by_hash(path):
    """A point that does not name the exact bytes it measured cannot be
    invalidated when the model changes, and a stale exact number is worse than
    no number: the customer trusts it precisely because it says `bench`."""
    sha = _load(path).get("model", {}).get("sha256", "")
    assert len(sha) == 64 and all(c in "0123456789abcdef" for c in sha), (
        f"{path.name}: model.sha256 must be a full lowercase sha256")


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_every_perf_point_pins_its_toolchain_version(path):
    tc = _load(path).get("toolchain", {})
    assert tc.get("name") and tc.get("version"), (
        f"{path.name}: a measurement is only valid for the toolchain that "
        f"produced it")


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_no_perf_point_claims_an_unmeasured_latency(path):
    """`runs` is what separates a measurement from a guess."""
    m = _load(path).get("measured", {})
    if "latency_ms_mean" in m:
        assert m.get("runs", 0) >= 1, (
            f"{path.name}: latency without a run count is not a measurement")


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_every_perf_point_validates_against_the_shipped_schema(path):
    validator = jsonschema.Draft202012Validator(_load(_SCHEMA_PATH))
    errors = sorted(validator.iter_errors(_load(path)),
                    key=lambda e: list(e.absolute_path))
    assert not errors, [
        f"{'/'.join(str(p) for p in e.absolute_path) or '<root>'}: {e.message}"
        for e in errors]


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_every_perf_point_passes_the_shipped_semantic_checks(path):
    """The real gate function, against the real checkout -- not a copy of its
    rules re-stated here, which could agree with a wrong implementation."""
    assert not V._check_model_perf_semantics([path])


#: A `<store>:<path>` citation or a URL, as opposed to a repo-relative path.
#: The SAME expression `validate_metadata._STORE_CITATION` uses, and it is
#: applied only AFTER `_LOCAL_PATH_REFERENCE` has rejected the local-disk
#: shapes, so `C:\models\x.tflite` never reaches it to be read as a store
#: named `C`.
_STORE_CITATION = V._STORE_CITATION


def _resolves_in_repo(source: str) -> Path | None:
    """The in-repo file `source` names, or None if it does not name one.

    Resolved and re-checked against `_ROOT` rather than trusted: a `source` of
    `../../etc/passwd` must not be read just because `_ROOT / source` happens
    to exist somewhere on the machine running the suite.
    """
    try:
        target = (_ROOT / source).resolve()
    except (OSError, ValueError):
        return None
    if not target.is_relative_to(_ROOT.resolve()) or not target.is_file():
        return None
    return target


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_model_source_is_a_citation_or_bytes_that_still_hash_to_the_pinned_sha256(path):
    """`model.source` has exactly two legal shapes, and the check that
    distinguishes them decides whether this tier can be campaigned at all.

    The first version of this test asked "does the string contain a slash?"
    and hard-failed if it did while no in-repo file matched.  That inverts the
    intent: `alp-sdk-internal:models/person_detect_int8.tflite` -- the citation
    form `capture.reference` MANDATES -- and `https://example.org/zoo/x.tflite`
    both failed, while the worthless `"somewhere"` passed.  Most of the model
    zoo is licence-gated or out of tree, so a point could not cite the models
    the campaign is FOR.

    What actually matters is reachability, so that is what is tested:

      * a `<store>:<path>` citation or a URL is a citation.  Its bytes are not
        here to be hashed, and that is legitimate.
      * anything else is a repo-relative path.  It MUST resolve under the
        checkout, and its bytes MUST still hash to `model.sha256` and match
        `model.size_bytes` -- which catches a fixture regenerated without
        re-benching, where the point keeps quoting arena/latency figures for
        bytes that no longer exist.  A path that does not resolve is a moved
        model or a typo, and both end traceability, so it fails rather than
        skipping.  `"somewhere"` fails here, correctly.

    This check lives HERE and not in `validate_metadata.py` on purpose.
    `model.source` points into `tests/fixtures/`, which does not exist in the
    metadata-ONLY scratch clone `test_alp_cli_new_som.py::_clone_metadata_gates`
    runs that gate against, so a copy there could only ever be a silent skip --
    the same split, for the same reason, as `_check_soc_vela_memory_profile`'s
    `source` citations.  This suite always runs against the real checkout.
    (The local-path REFUSAL does live in the gate: it needs no bytes to decide.)
    """
    doc = _load(path)
    source = doc.get("model", {}).get("source")
    assert source, f"{path.name}: model.source is required -- a sha256 with no " \
                   f"provenance could belong to any model"
    assert not V._LOCAL_PATH_REFERENCE.search(source), (
        f"{path.name}: model.source `{source}` names one machine's disk")
    if _STORE_CITATION.match(source):
        return  # a citation; the bytes are deliberately not in this repo
    target = _resolves_in_repo(source)
    assert target is not None, (
        f"{path.name}: model.source `{source}` is not a `<store>:<path>` "
        f"citation, so it must be a repo-relative path -- and no such file "
        f"resolves under the checkout")
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    assert digest == doc["model"]["sha256"], (
        f"{path.name}: model.source `{source}` now hashes to {digest}, but the "
        f"point was measured on {doc['model']['sha256']} -- the model changed "
        f"and the figures were not re-benched")
    assert target.stat().st_size == doc["model"]["size_bytes"], (
        f"{path.name}: model.size_bytes disagrees with `{source}` on disk")


@pytest.mark.parametrize("source", [
    "alp-sdk-internal:models/person_detect_int8.tflite",
    "https://example.org/zoo/x.tflite",
])
def test_a_licence_gated_or_out_of_tree_model_can_be_cited(source):
    """The regression guard for the inversion above, stated as the property
    that broke: the citation form the recipe mandates, and a URL, must be
    accepted without any bytes present.  Most of the zoo is one of these two;
    a rule that refuses them makes tier 2 uncampaignable."""
    assert _STORE_CITATION.match(source)
    assert not V._LOCAL_PATH_REFERENCE.search(source)


@pytest.mark.parametrize("source", [
    "/home/user/models/x.tflite",
    "C:\\models\\x.tflite",
    "OneDrive/models/x.tflite",
])
def test_a_local_disk_model_source_is_refused_before_it_looks_like_a_store(source):
    """Before the store allowlist, `C:\\models\\x.tflite` ALSO satisfied the
    `<store>:<path>` shape, with a store literally named `C` -- the allowlist
    now closes that on its own (`C` is not `alp-sdk-internal` / `https` /
    `http`), but `_LOCAL_PATH_REFERENCE` was always the real defence: it runs
    first, in both this suite and the shipped gate, and refuses all three of
    these regardless of what the citation pattern says."""
    assert V._LOCAL_PATH_REFERENCE.search(source)


@pytest.mark.parametrize("source", [
    "https:C:\\Users\\user\\log.txt",
    "http:D:\\bench\\run.log",
])
def test_a_drive_letter_path_after_a_store_is_still_a_local_disk_leak(source):
    """The residual this fix closes: `_LOCAL_PATH_REFERENCE`'s drive-letter
    alternative used to be anchored (`^[A-Za-z]:[/\\\\]`), so it only ever
    caught a LEADING drive letter -- a drive path tacked on AFTER a
    legitimate-looking store (`https:C:\\Users\\user\\log.txt`) was invisible
    to it, even though it names one developer's machine exactly as surely as
    a leading `C:\\` does. Measured accepted (rc=0) before this fix; must be
    refused now."""
    assert V._LOCAL_PATH_REFERENCE.search(source), (
        f"{source!r}: a drive-letter path after a store prefix must still be "
        f"refused as a local-machine leak")


@pytest.mark.parametrize("source", [
    "https://example.org/zoo/x.tflite",
    "http://example.org/zoo/x.tflite",
    "alp-sdk-internal:bench/captures/2026-08-16-aen801-person-detect.log",
    # The regression these four probe: a single-letter PATH or QUERY token
    # followed by `:/` -- none of the three cases above contain one, so none
    # of them can trip the over-reach this test is named for.  A word-
    # boundary guard (the letter must sit at the string start or after a
    # non-alphanumeric character) does NOT discriminate these from a real
    # drive letter: the `a` in `.../a:/b.log` sits right after a `/`, a
    # non-alphanumeric character, so it satisfies that guard exactly as a
    # genuine `C:\` does. These must be ACCEPTED as legitimate citations.
    "https://example.org/a:/b.log",
    "https://example.org/bench/c:/run.log",
    "https://example.org/x?q=a:/b",
    "alp-sdk-internal:a/b:/c.log",
])
def test_the_unanchored_drive_letter_check_does_not_false_positive_on_a_real_url(source):
    """The trap the naive fix falls into: unanchoring `[A-Za-z]:[/\\\\]`
    outright would match the `s` immediately before `://` in `https://...`
    (a single letter followed by `:` then `/`), refusing every legitimate URL
    citation. What actually keeps a real `https://`/`http://` URL from
    matching is NOT a word-boundary guard on the letter -- the `a` in
    `https://example.org/a:/b.log` sits right after a `/`, a non-
    alphanumeric character, so a word-boundary guard would wrongly accept it
    as a drive letter too, and DID (this exact false positive was measured
    accepted, i.e. wrongly refused as a local path, before this fix).  What
    actually discriminates is the SEPARATOR and its position: `[A-Za-z]:\\`
    (backslash) is checked anywhere, but `[A-Za-z]:/` (forward slash) is
    checked only at the string start or immediately after the store colon --
    never in the middle of a path or query segment, which is where all four
    of the colon-in-path cases below put theirs."""
    assert not V._LOCAL_PATH_REFERENCE.search(source), (
        f"{source!r}: a legitimate citation must not be flagged as a local "
        f"machine path")


def test_https_and_http_citations_require_the_scheme_separator():
    """A residual `changelog.d/1520.md`'s "Left open, deliberately" section
    documented, then this fix closed. `_STORE_CITATION` used to accept
    `<store>:` plus any single non-space character for EVERY store, so
    `https:findit` and `http:x` -- a colon with no authority slashes, which
    names an allowlisted store but is not a URL (the schema's own
    `capture.reference` example is `https://example.org/...`, never
    `https:...`) -- read as citations and skipped the reachability +
    sha256/size_bytes re-hash a repo-relative `model.source` gets. As a
    `model.source`, that is not a citation that merely fails to resolve --
    it turns the model-bytes integrity check off for a fabricated `sha256`.
    The same gap let `https:/home/user/x.log` (a single slash, not
    `https://`'s double) pass too, even though it is not a drive-letter path
    and not a leading local path, so `_LOCAL_PATH_REFERENCE` never caught it
    either. `https`/`http` now require their own scheme separator (`://`);
    `alp-sdk-internal` is not URL-shaped and keeps its bare-colon citation
    form, since a per-store shape table is not needed to fix this -- only a
    per-store SEPARATOR is."""
    for source in ("https:findit", "http:x", "https:/home/user/x.log"):
        assert not V._STORE_CITATION.match(source), (
            f"{source!r}: a bare colon (no `://`) must not satisfy the "
            f"https/http citation form")
        assert not V._LOCAL_PATH_REFERENCE.search(source), (
            f"{source!r}: not a local-machine path either -- this is the "
            f"store-allowlist gap, not the local-path refusal, that closes it")
    # `alp-sdk-internal` is unaffected: it is not URL-shaped, so a bare
    # colon is its correct and only citation form.
    assert V._STORE_CITATION.match("alp-sdk-internal:findit")


def test_a_bare_word_model_source_is_not_provenance():
    """`"somewhere"` used to PASS while a real citation failed.  It is neither
    a citation nor a path that resolves, so it must fail."""
    assert not _STORE_CITATION.match("somewhere")
    assert _resolves_in_repo("somewhere") is None


@pytest.mark.parametrize("source", [
    "todo:findit",
    "x:y",
    "ask:Caner",
    "note:see the log",
])
def test_a_colon_bearing_non_store_is_not_a_citation(source):
    """The MAJOR hole this fix closes.  `_STORE_CITATION` used to be
    `^[A-Za-z0-9][A-Za-z0-9._+-]*:\\S`, which routes ANY colon-bearing string
    down the citation branch and skips the reachability + sha256/size_bytes
    re-hash a repo-relative `model.source` gets.  Every one of these four
    strings is exactly what the reviewer measured as ACCEPTED-and-never-
    re-hashed against the shipped gate at f724d3e4 -- `note:see the log` and
    `ask:Caner` are the sharpest of the four: they are the very `see the log`
    / `ask Caner` shapes `metadata/schemas/model-perf-v1.schema.json`'s
    `capture.reference` description names as what the denylist-turned-
    allowlist was supposed to keep out, still admitted the moment a colon
    follows.  Constraining the STORE SEGMENT to the three that legitimately
    appear (`alp-sdk-internal`, `https`, `http`) closes it: none of these four
    names one, so none is a citation any more."""
    assert not V._STORE_CITATION.match(source)


def test_store_citation_prefixes_cover_every_store_name():
    """`_STORE_CITATION_PREFIXES` is derived from `_STORE_NAMES` +
    `_STORE_SEPARATORS`, never a second hand-typed store list, precisely so
    the store count cannot drift between the bare-name list
    `_LOCAL_PATH_REFERENCE` reads and the full-prefix list `_STORE_CITATION`
    reads. A name missing from `_STORE_SEPARATORS` already raises `KeyError`
    at import time rather than silently compiling a separator-less
    alternative; this test is the readable, always-collected form of that
    same guarantee, bound to the LIVE values on both sides."""
    assert set(V._STORE_SEPARATORS) == set(V._STORE_NAMES), (
        "_STORE_SEPARATORS must declare exactly the entries _STORE_NAMES "
        "does -- neither more nor fewer")
    assert V._STORE_CITATION_PREFIXES == tuple(
        name + V._STORE_SEPARATORS[name] for name in V._STORE_NAMES), (
        "_STORE_CITATION_PREFIXES has drifted from _STORE_NAMES + "
        "_STORE_SEPARATORS")


def test_model_source_and_capture_reference_agree_on_the_store_allowlist():
    """The two expressions used to disagree on `+`: `_STORE_CITATION` allowed
    it (`[A-Za-z0-9._+-]*`) while the schema's `capture.reference` pattern did
    not (`[A-Za-z0-9._-]*`), so `git+ssh:models/x.tflite` was a valid
    `model.source` citation and an invalid `capture.reference`.  JSON Schema
    cannot `$ref` a Python constant, so there is no single definition either
    side reads -- this test IS the lockstep: it fails the moment an edit to
    one store allowlist is not mirrored in the other, rather than the two
    drifting apart silently."""
    schema = json.loads(_SCHEMA_PATH.read_text(encoding="utf-8"))
    schema_pattern = schema["properties"]["capture"]["properties"]["reference"]["pattern"]
    assert schema_pattern == V._STORE_CITATION.pattern, (
        f"metadata/schemas/model-perf-v1.schema.json capture.reference.pattern "
        f"{schema_pattern!r} disagrees with validate_metadata._STORE_CITATION.pattern "
        f"{V._STORE_CITATION.pattern!r}")


#: ECMA-262 escapes of a non-special character with a backslash outside a
#: character class ARE a `SyntaxError` under `u`/`v` mode, but Python's `re`
#: accepts them via Annex-B leniency -- exactly the gap `re.escape("alp-sdk-
#: internal")` fell into, shipping `alp\-sdk\-internal` in both this pattern
#: and the JSON Schema it must stay byte-identical to.  A JS/TS consumer that
#: compiles JSON Schema `pattern` strings under unicode mode by default (Ajv's
#: `unicodeRegExp: true`, which alp-sdk-vscode's Ajv inherits) could not
#: compile the schema AT ALL while that escape shipped.
_ECMA262_RECOGNISED_ESCAPES = set("^$\\.*+?()[]{}|/bBdDsSwWnrtfv0123456789ukxc")


def _non_ecma262_escapes(pattern: str) -> list[str]:
    """Every `\\X` in `pattern` where X is not one of ECMA-262's recognised
    escape targets AND the escape sits outside a `[...]` character class
    (escaping a `-` is legal INSIDE one). Returns the empty list for a
    pattern with no such escape."""
    bad = []
    in_class = False
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\" and i + 1 < len(pattern):
            nxt = pattern[i + 1]
            if not in_class and nxt not in _ECMA262_RECOGNISED_ESCAPES:
                bad.append(pattern[i:i + 2])
            i += 2
            continue
        if c == "[":
            in_class = True
        elif c == "]":
            in_class = False
        i += 1
    return bad


def test_store_citation_pattern_has_no_non_ecma262_escape():
    """Regression guard for the exact defect fixed here, bound to the LIVE
    pattern (never a hand-copied literal) so a future edit is checked
    automatically -- `_STORE_CITATION` used to read `alp\\-sdk\\-internal`, an
    escaped `-` OUTSIDE a character class."""
    live = V._STORE_CITATION.pattern
    bad = _non_ecma262_escapes(live)
    assert not bad, (
        f"validate_metadata._STORE_CITATION.pattern {live!r} escapes {bad} "
        f"outside a character class -- invalid under ECMA-262")

    # Mutation proof: reintroducing the exact defect on the live pattern must
    # be caught by the same helper.
    mutated = live.replace("alp-sdk-internal", "alp\\-sdk\\-internal")
    assert mutated != live, "mutation did not change the pattern -- test is vacuous"
    assert _non_ecma262_escapes(mutated) == ["\\-", "\\-"], (
        f"the helper failed to flag the reintroduced defect in {mutated!r}")

    # Dropping `re.escape` when building `_STORE_CITATION` (and the
    # identically-built schema `pattern`) is correct only as long as every
    # `_STORE_NAMES` entry is itself free of regex metacharacters -- bound
    # to the LIVE tuple, never a hand-copied literal, because a future
    # entry (`git+ssh`, `s3.amazonaws.com`) would compile `+`/`.` as
    # metacharacters on BOTH sides identically, so the lockstep test above
    # would stay green while both sides were silently wrong. Checked against
    # the metacharacter set that actually matters at the position
    # `_STORE_NAMES` is joined into -- OUTSIDE a `[...]` class -- rather
    # than via `re.escape`, which is not usable here: Python's `re.escape`
    # escapes `-` unconditionally (`re.escape("alp-sdk-internal") ==
    # "alp\\-sdk\\-internal"`), the exact escaped-outside-a-class shape this
    # whole fix exists to keep OUT of the shipped pattern, so it would flag
    # today's real, correct store name as if it were the defect.
    _outside_class_metachars = set(".^$*+?{}[]\\|()")
    for name in V._STORE_NAMES:
        bad_chars = sorted(set(name) & _outside_class_metachars)
        assert not bad_chars, (
            f"_STORE_NAMES entry {name!r} contains regex metacharacter(s) "
            f"{bad_chars} -- _STORE_CITATION joins _STORE_NAMES WITHOUT "
            f"re.escape, so these would compile as metacharacters rather "
            f"than literals")

    # Mutation proof: a metacharacter-bearing name must trip the assertion
    # above -- confirms it is not vacuously true for every string.
    assert set("git+ssh") & _outside_class_metachars, (
        "mutation did not introduce a metacharacter -- test is vacuous")


def test_store_citation_pattern_compiles_as_an_ecma262_regexp():
    """The strongest form of the guard: actually compile the shipped pattern
    as a `RegExp` under Node's unicode mode (`'u'` flag), the same mode Ajv
    (and so alp-sdk-vscode's Ajv-based JSON Schema consumer) uses by default.
    Skipped, not failed, when `node` is not on PATH -- `test_store_citation_
    pattern_has_no_non_ecma262_escape` above is the portable fallback that
    always runs."""
    node = shutil.which("node") or shutil.which("nodejs")
    if node is None:
        pytest.skip("node not available on PATH")
    script = (
        "try { new RegExp(process.argv[1], 'u'); process.exit(0); } "
        "catch (e) { console.error(e.message); process.exit(1); }"
    )
    result = subprocess.run(
        [node, "-e", script, V._STORE_CITATION.pattern],
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (
        f"validate_metadata._STORE_CITATION.pattern {V._STORE_CITATION.pattern!r} "
        f"does not compile as an ECMA-262 RegExp under unicode mode: "
        f"{result.stderr.strip()}")


def test_no_published_point_is_a_synthetic_fixture():
    """`metadata/model_perf/` is bench data.  A `_fixture`-marked document
    carries placeholder figures and must never be readable as a measurement.
    (Iterates nothing today -- the `fixture-marker-in-the-published-tree`
    mutation is what proves the rule bites; this catches a fixture COPIED in
    later, whether or not anyone runs the mutation suite.)"""
    for path in _points(_PUBLISHED):
        assert "_fixture" not in _load(path), (
            f"{path.name}: carries `_fixture`, so its `measured` values are "
            f"placeholders, not measurements")


@pytest.mark.parametrize("path", _points(_FIXTURES), ids=_ids)
def test_every_test_fixture_declares_itself_a_fixture(path):
    """The other half: a fixture that loses its `_fixture` marker becomes a
    document indistinguishable from a real measurement, and nothing would stop
    it being moved into the published tree."""
    assert "_fixture" in _load(path), (
        f"{path.name}: a synthetic perf point must say so")


# ---------------------------------------------------------------------------
# The target-resolution helper the accel_config check rests on.
# ---------------------------------------------------------------------------

def test_every_shipping_sku_resolves_at_least_the_cpu_target():
    skus = sorted(p.stem for p in (_ROOT / "metadata" / "e1m_modules").glob("E1M-*.yaml"))
    assert skus, "no SoM presets found"
    for sku in skus:
        targets = V._resolve_perf_targets(sku, _ROOT / "metadata")
        assert ("cpu", "") in targets, f"{sku}: CPU is always a target"


def test_aen801_resolves_its_three_ethos_u_targets():
    """Pinned against what `tan.model.targets.resolve_targets` itself produces
    for this SKU (measured 2026-08-16 against this checkout's metadata), since
    alp-sdk cannot import tan and both sides must agree on what
    `metadata/socs/alif/ensemble/e8.json`'s `npus[]` means.  E1M-AEN801 is the
    first campaign's SKU, so this is the set the first real points will claim.
    """
    assert V._resolve_perf_targets("E1M-AEN801", _ROOT / "metadata") == {
        ("cpu", ""),
        ("ethos_u", "ethos-u55-128"),
        ("ethos_u", "ethos-u55-256"),
        ("ethos_u", "ethos-u85-256"),
    }


def test_v2m101_picks_up_its_discrete_deepx_accelerator():
    """The subtle branch: the DX-M1 is not in the RZ/V2N's own `npus[]`.  It is
    found by scanning every OTHER SoC spec for one whose
    `variants[].alp_module_skus` lists this SKU -- so `metadata/socs/**` stays
    the single source of truth and no backend->SKU table is hand-kept."""
    assert V._resolve_perf_targets("E1M-V2M101", _ROOT / "metadata") == {
        ("cpu", ""), ("drpai", ""), ("deepx_dxm1", ""),
    }


def test_target_resolution_fails_closed_on_an_unknown_sku():
    """It must RAISE, not return a partial or empty set: the caller reports
    "this SKU does not have that target" as a hard failure, so a quiet empty
    answer would reject a legitimate point on the strength of a file we could
    not read -- and a quiet full answer would accept anything."""
    with pytest.raises(LookupError):
        V._resolve_perf_targets("E1M-AEN999", _ROOT / "metadata")


# ---------------------------------------------------------------------------
# The core-resolution helper, and the NPU<->core pairing it can enforce.
# ---------------------------------------------------------------------------

def test_every_shipping_sku_declares_at_least_one_core():
    skus = sorted(p.stem for p in (_ROOT / "metadata" / "e1m_modules").glob("E1M-*.yaml"))
    assert skus, "no SoM presets found"
    for sku in skus:
        assert V._resolve_perf_cores(sku, _ROOT / "metadata"), f"{sku}: no cores"


def test_aen801_and_v2n101_resolve_the_cores_their_topology_declares():
    """Pinned against this checkout's `topology:` maps.  The core is in the
    measurement identity because it changes the number: an E1M-AEN801 `cpu`
    point on `a32_cluster` and the same model on `m55_he` are not comparable,
    and without the core every `cpu` point on one SKU collides on one path."""
    meta = _ROOT / "metadata"
    assert V._resolve_perf_cores("E1M-AEN801", meta) == {
        "a32_cluster", "m55_hp", "m55_he"}
    assert V._resolve_perf_cores("E1M-V2N101", meta) == {"a55_cluster", "m33_sm"}


def test_core_resolution_fails_closed_on_an_unknown_sku():
    with pytest.raises(LookupError):
        V._resolve_perf_cores("E1M-AEN999", _ROOT / "metadata")


def test_the_pairing_is_enforced_only_where_the_soc_declares_one():
    """The honest half of the core rule.  `metadata/socs/alif/ensemble/e8.json`
    declares `paired_core` on each Ethos-U55 (`m55_hp` for the high-perf,
    `m55_he` for the high-efficiency) and declares NONE on the Ethos-U85.  So
    the gate may pin the U55 points to a core and must NOT invent one for the
    U85 -- the metadata does not know, and a guess here would be a hardware
    claim wearing a validator's authority."""
    pairs = V._perf_target_map("E1M-AEN801", _ROOT / "metadata")
    assert pairs[("ethos_u", "ethos-u55-256")] == "m55_hp"
    assert pairs[("ethos_u", "ethos-u55-128")] == "m55_he"
    assert pairs[("ethos_u", "ethos-u85-256")] is None
    assert pairs[("cpu", "")] is None


def test_perf_target_map_survives_a_non_object_variants_entry_on_another_soc(tmp_path):
    """`_perf_target_map` walks EVERY OTHER SoC spec (not just the SKU's host)
    looking for an on-module discrete accelerator declared via
    `variants[].alp_module_skus` -- the DEEPX DX-M1 on the V2M SKUs is the
    shipping example: `metadata/socs/deepx/dx/m1.json` names `E1M-V2M101` and
    `E1M-V2M102` there, while E1M-V2M101's own host SoC spec is
    `renesas:rzv2n:n44`.  That second loop built its `skus` set with a bare
    `v.get("alp_module_skus")`, unguarded by an `isinstance(v, dict)` filter
    unlike every sibling `variants[]` read in this file (`_check_soc_npu_
    pairing`, `_check_soc_vela_memory_profile`, `_check_soc_debug_probe_
    identity`, the Ensemble `jlink_flash_device` and no-WLCSP checks) -- a
    non-object entry on ANY non-host SoC spec raised an unhandled
    `AttributeError: 'str' object has no attribute 'get'` here, aborting the
    whole gate before `_check_model_perf_semantics`'s `except LookupError` at
    the call site could catch it (that clause does not catch `AttributeError`
    at all)."""
    root = tmp_path / "scratch"
    for sub in ("socs", "e1m_modules"):
        shutil.copytree(_ROOT / "metadata" / sub, root / "metadata" / sub)
    m1 = root / "metadata" / "socs" / "deepx" / "dx" / "m1.json"
    doc = json.loads(m1.read_text(encoding="utf-8"))
    doc["variants"] = ["not-a-dict"] + doc["variants"]
    m1.write_text(json.dumps(doc), encoding="utf-8")
    pairs = V._perf_target_map("E1M-V2M101", root / "metadata")  # must not raise
    assert pairs[("cpu", "")] is None


# ---------------------------------------------------------------------------
# The toolchain-profile digest that closes the second half of blocker 2.
# ---------------------------------------------------------------------------

def test_the_profile_digest_separates_two_vela_profiles():
    """`Ethos_U85_SRAM_Only` and `Ethos_U85_SYS_DRAM_Mid` are different
    machines -- the second is DRAM-backed and the part has no DRAM -- so two
    points measured under them must not resolve to one filename, where the
    survivor is whichever was written last."""
    sram = V._toolchain_profile_digest({
        "name": "vela", "version": "5.1.0",
        "system_config": "Ethos_U85_SRAM_Only", "memory_mode": "Sram_Only"})
    dram = V._toolchain_profile_digest({
        "name": "vela", "version": "5.1.0",
        "system_config": "Ethos_U85_SYS_DRAM_Mid",
        "memory_mode": "Dedicated_Sram_384KB"})
    assert sram != dram
    assert len(sram) == 12 and all(c in "0123456789abcdef" for c in sram)


def test_the_profile_digest_ignores_name_and_version_and_key_order():
    """`name` and `version` are already literal segments of the filename, so
    folding them in again would only make the digest change when the readable
    part already did.  Key order must not matter: the same profile written two
    ways is one profile."""
    a = V._toolchain_profile_digest(
        {"name": "vela", "version": "5.1.0", "memory_mode": "Sram_Only",
         "system_config": "Ethos_U85_SRAM_Only"})
    b = V._toolchain_profile_digest(
        {"system_config": "Ethos_U85_SRAM_Only", "memory_mode": "Sram_Only",
         "name": "dxcom", "version": "2.3.0"})
    assert a == b


def test_the_profile_digest_covers_toolchain_pins():
    """`pins` records version/config pins alp-sdk does not override (DRP-AI's
    `drp_compiler_version`).  They change the compile, so they change the
    identity: the digest is derived as "every key except name and version"
    rather than from a hard-coded list, so a profile key added to the schema
    later enters the identity automatically."""
    bare = V._toolchain_profile_digest({"name": "drpai-tvm", "version": "2.4.0"})
    pinned = V._toolchain_profile_digest({
        "name": "drpai-tvm", "version": "2.4.0",
        "pins": {"drp_compiler_version": "100"}})
    assert bare != pinned


def test_the_fixture_filename_carries_the_digest_of_its_own_profile():
    """The base fixture's stem is not hand-typed: it is what the shipped
    helper produces for the profile inside it."""
    doc = _load(_BASE)
    assert _BASE.stem.endswith("+" + V._toolchain_profile_digest(doc["toolchain"]))


# ---------------------------------------------------------------------------
# A non-object `npu_toolchain` must not crash the gate with a traceback, and
# an ethos_u point against a SoC that declares NO npu_toolchain block at all
# must be refused rather than silently passed.
# ---------------------------------------------------------------------------

def test_check_soc_vela_memory_profile_survives_a_non_object_npu_toolchain(tmp_path):
    """`(doc.get("npu_toolchain") or {}).get("vela")` assumed a mapping.  A
    non-object `npu_toolchain` (e.g. authored as a list) used to raise an
    unhandled `AttributeError: 'list' object has no attribute 'get'`, proven
    against the shipped gate at f724d3e4 -- AFTER the schema pass had already
    printed the real `FAIL ... npu_toolchain: [...] is not of type 'object'`
    line, so the traceback hid a failure the gate had already found.  The
    schema catching it separately does not excuse this function crashing;
    they run as two independent passes over the same file."""
    soc = tmp_path / "bogus.json"
    soc.write_text(json.dumps({
        "ref": "bogus/soc",
        "npus": [{"type": "ethos-u55", "subtype": "high-perf"}],
        "npu_toolchain": ["vela"],
    }), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", tmp_path)
        failures = V._check_soc_vela_memory_profile([soc])  # must not raise
    assert failures and any(
        "no npu_toolchain.vela" in m for _, msgs in failures for m in msgs), (
        f"expected a `no npu_toolchain.vela` complaint, got {failures!r}")


def test_check_soc_vela_memory_profile_survives_a_non_object_vela_block(tmp_path):
    """The sibling the guard above did NOT close.  Guarding `npu_toolchain`
    itself (above) is not the same as guarding `npu_toolchain.vela` -- a
    version STRING authored where the vela profile OBJECT belongs
    (`{"vela": "5.1.0"}`) is still a truthy non-dict, so `.get("vela") or {}`
    passes it straight through and the very next line, `vela.get(
    "memory_mode")`, raised an unhandled `AttributeError: 'str' object has
    no attribute 'get'`, proven against the shipped gate. Same shape, same
    fix, one guard short: `vela` must be normalised the same way
    `npu_toolchain` is, not merely defaulted on falsy."""
    soc = tmp_path / "bogus.json"
    soc.write_text(json.dumps({
        "ref": "bogus/soc",
        "npus": [{"type": "ethos-u85", "subtype": "generative"}],
        "npu_toolchain": {"vela": "5.1.0"},
    }), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", tmp_path)
        failures = V._check_soc_vela_memory_profile([soc])  # must not raise
    assert failures and any(
        "no npu_toolchain.vela" in m for _, msgs in failures for m in msgs), (
        f"expected a `no npu_toolchain.vela` complaint, got {failures!r}")


def test_check_soc_vela_memory_profile_survives_non_object_list_entries(tmp_path):
    """The two OTHER unguarded `.get()` calls in the same function, both one
    or two lines from the `npu_toolchain` guard: `npus[]` entries (used to
    build `ethos`) and `external_memory_interfaces[]` entries (used to build
    `kinds`) are schema-typed lists of objects, but nothing stopped a
    non-object entry in either from reaching a bare `.get()` and raising
    `AttributeError` here, same as `vela` above. A doc with malformed entries
    in ALL THREE places at once must still return cleanly rather than crash
    on the first one reached."""
    soc = tmp_path / "bogus.json"
    soc.write_text(json.dumps({
        "ref": "bogus/soc",
        "npus": ["not-a-dict"],
        "npu_toolchain": {"vela": {"memory_mode": "Dedicated_Sram_384KB"}},
        "external_memory_interfaces": ["not-a-dict"],
    }), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", tmp_path)
        failures = V._check_soc_vela_memory_profile([soc])  # must not raise
    # `npus` filtered to dicts is empty, so `ethos` is empty too, and `vela`
    # (a real object) with no `ethos` NPU trips rule (1)'s converse -- the
    # important assertion is that this line was reached at all, not raised.
    assert failures and any(
        "no ethos-u* NPU" in m for _, msgs in failures for m in msgs), (
        f"expected a `no ethos-u* NPU` complaint, got {failures!r}")


@pytest.fixture
def _host_soc_with_non_object_npu_toolchain(tmp_path):
    """A scratch `metadata/` copy with E1M-AEN801's host SoC spec
    (`metadata/socs/alif/ensemble/e8.json`)'s `npu_toolchain` replaced by a
    list, for `_soc_npu_toolchain_names` / the rule-6 toolchain.name
    cross-check.  Function-scoped and independent of the module-scoped `gate`
    fixture above, since this mutates a SoC file rather than a perf point and
    must not leak into the other mutation cases sharing that fixture."""
    root = tmp_path / "scratch"
    for sub in ("socs", "e1m_modules"):
        shutil.copytree(_ROOT / "metadata" / sub, root / "metadata" / sub)
    e8 = root / "metadata" / "socs" / "alif" / "ensemble" / "e8.json"
    doc = json.loads(e8.read_text(encoding="utf-8"))
    doc["npu_toolchain"] = ["vela"]
    e8.write_text(json.dumps(doc), encoding="utf-8")
    return root


def test_toolchain_name_cross_check_survives_a_non_object_npu_toolchain(
        _host_soc_with_non_object_npu_toolchain):
    """The same guard, exercised end to end through
    `_check_model_perf_semantics`: an `ethos_u` point against a host SoC whose
    `npu_toolchain` is a list must not crash
    (`validate_metadata.py`'s `_soc_npu_toolchain_names`)."""
    doc = _load(_BASE)
    doc.pop("_fixture", None)
    path = _host_soc_with_non_object_npu_toolchain / _BASE_REL
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", _host_soc_with_non_object_npu_toolchain)
        failures = V._check_model_perf_semantics([path])  # must not raise
    assert failures, "a non-object npu_toolchain must not be silently accepted"


def test_an_ethos_u_point_against_a_soc_with_no_npu_toolchain_block_is_refused(
        _host_soc_with_non_object_npu_toolchain):
    """Rule 6's fail-closed contract: `if known_toolchains and tc_name not in
    known_toolchains:` used to skip silently whenever the SoC declared no
    `npu_toolchain` block (empty OR non-object both resolve to an empty
    `known_toolchains` set) -- measured accepted (`OK`) at rc=0 with e8.json's
    block removed, and again with it set to `{}`, while every sibling rule
    (2/3/4/5) in this same function refuses an unresolvable SKU rather than
    pass over it.  A non-object `npu_toolchain` (this fixture) exercises the
    identical empty-set path `_soc_npu_toolchain_names` takes for a MISSING
    block, so the same message must fire."""
    doc = _load(_BASE)
    doc.pop("_fixture", None)
    path = _host_soc_with_non_object_npu_toolchain / _BASE_REL
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", _host_soc_with_non_object_npu_toolchain)
        failures = V._check_model_perf_semantics([path])
    assert failures and any(
        "declares no npu_toolchain block at all" in m
        for _, msgs in failures for m in msgs), (
        f"expected a `declares no npu_toolchain block at all` refusal, got "
        f"{failures!r}")


@pytest.fixture
def _scratch_metadata_with_malformed_hw_revisions(tmp_path):
    """A scratch `metadata/` copy with the `aen` family's
    `hw-revisions.yaml` replaced by a top-level LIST, for rule 4's
    `measured_on.hw_rev` cross-check. `strict_yaml_load` is a mapping in
    every valid hw-revisions.yaml, but the schema pass that would reject a
    malformed one runs separately and is not guaranteed to have run first."""
    root = tmp_path / "scratch"
    for sub in ("socs", "e1m_modules"):
        shutil.copytree(_ROOT / "metadata" / sub, root / "metadata" / sub)
    table = root / "metadata" / "e1m_modules" / "aen" / "hw-revisions.yaml"
    table.write_text("- not\n- a\n- mapping\n", encoding="utf-8")
    return root


def test_hw_rev_cross_check_survives_a_non_mapping_hw_revisions_table(
        _scratch_metadata_with_malformed_hw_revisions):
    """`(revisions or {}).get("hw_revisions")` assumed a mapping. A
    top-level LIST in `hw-revisions.yaml` (e.g. authored with a stray `-`)
    used to raise an unhandled `AttributeError: 'list' object has no
    attribute 'get'` here, proven against the shipped gate. Same shape,
    same fix, as the SoC-spec guards this fix also applies."""
    doc = _load(_BASE)
    doc.pop("_fixture", None)
    path = _scratch_metadata_with_malformed_hw_revisions / _BASE_REL
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(V, "REPO", _scratch_metadata_with_malformed_hw_revisions)
        failures = V._check_model_perf_semantics([path])  # must not raise
    assert failures and any(
        "is not a revision of the" in m for _, msgs in failures for m in msgs), (
        f"expected a `is not a revision of the` refusal, got {failures!r}")


# ---------------------------------------------------------------------------
# Mutation proofs.  Each case asserts BOTH directions: the unmutated document
# passes the shipped gate, and the mutant fails it with the message that names
# the rule.
# ---------------------------------------------------------------------------

def _drop(doc: dict, *path: str) -> dict:
    node = doc
    for key in path[:-1]:
        node = node[key]
    del node[path[-1]]
    return doc


def _set(doc: dict, value, *path: str) -> dict:
    node = doc
    for key in path[:-1]:
        node = node[key]
    node[path[-1]] = value
    return doc


#: (id, relative path to place the document at, mutation, substring the gate's
#: complaint must contain).  `None` for the mutation means "unchanged", used to
#: prove the published shape passes.
_MUTATIONS = [
    # --- rule 1: the path is a claim the body must reproduce ---------------
    ("path-sku-dir-disagrees",
     f"metadata/model_perf/E1M-AEN601/ethos-u85-256/{_STEM}.json",
     None, "measured_on.sku"),
    ("path-target-dir-disagrees",
     f"metadata/model_perf/E1M-AEN801/ethos-u55-256/{_STEM}.json",
     None, "implies directory"),
    ("path-model-sha-disagrees",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-deadbeefcafe@vela-5.1.0+r2+m55_hp+1e562a678c9f.json",
     None, "imply filename"),
    ("path-toolchain-version-drifts-from-filename",
     None, lambda d: _set(d, "5.2.0", "toolchain", "version"), "imply filename"),
    ("path-toolchain-name-drifts-from-filename",
     None, lambda d: _set(d, "dxcom", "toolchain", "name"), "imply filename"),
    ("path-model-slug-drifts-from-filename",
     None, lambda d: _set(d, "person-detect", "model", "slug"), "imply filename"),
    # --- rule 1, the three segments blocker 2 added.  Each of these is a
    #     measurement that legitimately differs from the base one, so WITHOUT
    #     its segment in the stem it resolves to the base point's exact path
    #     and the second write destroys the first. -------------------------
    ("path-hw-rev-drifts-from-filename",
     None, lambda d: _set(d, "r1", "measured_on", "hw_rev"), "imply filename"),
    ("path-core-drifts-from-filename",
     None, lambda d: _set(d, "a32_cluster", "measured_on", "core"), "imply filename"),
    ("path-vela-system-config-drifts-from-filename",
     None, lambda d: _set(d, "Ethos_U85_SYS_DRAM_Mid", "toolchain", "system_config"),
     "imply filename"),
    ("path-vela-memory-mode-drifts-from-filename",
     None, lambda d: _set(d, "Dedicated_Sram_384KB", "toolchain", "memory_mode"),
     "imply filename"),
    ("path-toolchain-pins-drift-from-filename",
     None, lambda d: _set(d, {"drp_compiler_version": "100"}, "toolchain", "pins"),
     "imply filename"),
    # --- rule 2: the SKU exists --------------------------------------------
    ("sku-does-not-exist",
     f"metadata/model_perf/E1M-AEN999/ethos-u85-256/{_STEM}.json",
     lambda d: _set(d, "E1M-AEN999", "measured_on", "sku"), "no SoM preset"),
    # --- rule 3: the SKU really has that target ----------------------------
    ("accel-config-the-sku-does-not-have",
     f"metadata/model_perf/E1M-AEN801/ethos-u55-512/{_STEM}.json",
     lambda d: _set(d, "ethos-u55-512", "measured_on", "accel_config"),
     "is not a target"),
    ("backend-the-sku-does-not-have",
     "metadata/model_perf/E1M-AEN801/deepx_dxm1/"
     "person-detect-int8-808cfdfc0cf3@dxcom-2.3.0+r2+m55_hp+1e562a678c9f.json",
     lambda d: _set(_set(_set(_set(d, "deepx_dxm1", "measured_on", "backend"),
                              "", "measured_on", "accel_config"),
                         "dxcom", "toolchain", "name"),
                    "2.3.0", "toolchain", "version"),
     "is not a target"),
    # --- rule 4: the hardware revision exists ------------------------------
    ("hw-rev-not-in-the-family-table",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r99+m55_hp+1e562a678c9f.json",
     lambda d: _set(d, "r99", "measured_on", "hw_rev"),
     "is not a revision of the aen family"),
    # --- rule 5: the core exists, and honours a declared pairing -----------
    ("core-the-sku-does-not-declare",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m7+1e562a678c9f.json",
     lambda d: _set(d, "m7", "measured_on", "core"), "is not a core"),
    ("core-contradicts-the-soc-declared-pairing",
     "metadata/model_perf/E1M-AEN801/ethos-u55-128/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+1e562a678c9f.json",
     lambda d: _set(d, "ethos-u55-128", "measured_on", "accel_config"),
     "paired_core"),
    # --- rule 6: an Ethos-U point records its vela profile ------------------
    ("ethos-u-point-without-system-config",
     None, lambda d: _drop(d, "toolchain", "system_config"),
     "records no system_config"),
    ("ethos-u-point-without-memory-mode",
     None, lambda d: _drop(d, "toolchain", "memory_mode"),
     "records no memory_mode"),
    # --- rule 6, the toolchain.name/backend coherence half: a `backend:
    #     "ethos_u"` point compiled by `dxcom` (the DEEPX compiler) is
    #     incoherent, and toolchain.name is one of the eight consumer
    #     match-key fields, so this is not cosmetic.  `dxcom` replaces
    #     `vela` in the filename's `@<toolchain>-<version>` segment too --
    #     the profile digest excludes `name`/`version`, so it is unchanged --
    #     which keeps rule 1 (the path/body identity check) silent so this
    #     case tests only the rule under test. -------------------------------
    ("ethos-u-point-with-deepx-toolchain",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-808cfdfc0cf3@dxcom-5.1.0+r2+m55_hp+1e562a678c9f.json",
     lambda d: _set(d, "dxcom", "toolchain", "name"),
     "npu_toolchain block names"),
    # --- rule 7: p95 cannot undercut the mean ------------------------------
    ("p95-below-the-mean",
     None, lambda d: _set(d, 0.5, "measured", "latency_ms_p95"),
     "is below measured.latency_ms_mean"),
    # --- rule 8: a footprint that undercuts its own arena fits everything --
    ("req-sram-kib-of-zero-beside-a-real-arena",
     None, lambda d: _set(d, 0, "measured", "req_sram_kib"),
     "incapable of failing"),
    ("req-sram-kib-below-its-own-arena",
     None, lambda d: _set(_set(d, 74480, "measured", "arena_bytes"),
                          72, "measured", "req_sram_kib"),
     "below measured.arena_bytes"),
    # --- rule 9: the recipe's timed-run floor -------------------------------
    ("single-shot-dressed-as-a-measurement",
     None, lambda d: _set(_set(_set(d, 1, "measured", "runs"),
                               12.437, "measured", "latency_ms_mean"),
                          12.437, "measured", "latency_ms_p95"),
     "run floor"),
    ("run-count-just-under-the-floor",
     None, lambda d: _set(d, 99, "measured", "runs"), "run floor"),
    # --- rule 10: an ISO-shaped string is not necessarily a day -------------
    ("capture-date-is-not-a-calendar-day",
     None, lambda d: _set(d, "2026-13-45", "capture", "date"),
     "not a real calendar date"),
    # --- rule 11: a CPU point has no NPU to place an operator on ------------
    ("cpu-point-reporting-npu-ops",
     "metadata/model_perf/E1M-AEN801/cpu/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+44136fa355b3.json",
     lambda d: _set(_set(_set(_drop(_drop(d, "toolchain", "system_config"),
                                    "toolchain", "memory_mode"),
                              "cpu", "measured_on", "backend"),
                         "", "measured_on", "accel_config"),
                    44, "measured", "npu_ops"),
     "no accelerator on this path"),
    # --- rule 12: the published tree cannot absorb a fixture ----------------
    ("fixture-marker-in-the-published-tree",
     _PUBLISHED_REL, None, "must never ship under metadata/model_perf/"),
    # --- rule 13: capture.reference / model.source cite a store, not a disk -
    #     `/home/user/` and not a real-looking username on purpose: the
    #     literal lands in a committed file, and scripts/check_local_paths.py
    #     is a REQUIRED gate that refuses a hard-coded home path unless the
    #     user segment is a documented placeholder.  `user` is one of those
    #     placeholders and still exercises `_LOCAL_PATH_REFERENCE`'s `^[/\\]`
    #     branch, so the mutation keeps biting.
    ("capture-reference-is-a-posix-local-path",
     None, lambda d: _set(d, "/home/user/bench/2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    ("capture-reference-is-a-windows-local-path",
     None, lambda d: _set(d, "C:\\bench\\2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    ("capture-reference-is-a-onedrive-share",
     None, lambda d: _set(d, "OneDrive/bench/2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    ("model-source-is-a-windows-local-path",
     None, lambda d: _set(d, "C:\\models\\person_detect_int8.tflite", "model", "source"),
     "model.source"),
    ("model-source-climbs-out-of-the-checkout",
     None, lambda d: _set(d, "../models/person_detect_int8.tflite", "model", "source"),
     "climbs out of the checkout"),
    # --- the schema half of the same gate ----------------------------------
    ("capture-reference-is-not-a-citation-at-all",
     None, lambda d: _set(d, "see the log", "capture", "reference"), "reference"),
    ("capture-reference-says-n-a",
     None, lambda d: _set(d, "n/a -- no capture exists", "capture", "reference"),
     "reference"),
    ("model-source-dropped",
     None, lambda d: _drop(d, "model", "source"), "source"),
    ("core-dropped",
     None, lambda d: _drop(d, "measured_on", "core"), "core"),
    ("stance-claims-an-estimate",
     None, lambda d: _set(d, "estimated", "stance"), "stance"),
    ("model-sha256-truncated",
     None, lambda d: _set(d, "808cfdfc", "model", "sha256"), "sha256"),
    ("model-sha256-uppercase",
     None, lambda d: _set(d, d["model"]["sha256"].upper(), "model", "sha256"), "sha256"),
    ("latency-without-a-run-count",
     None, lambda d: _drop(d, "measured", "runs"), "runs"),
    ("run-count-of-zero",
     None, lambda d: _set(d, 0, "measured", "runs"), "runs"),
    ("nothing-measured-at-all",
     None, lambda d: _set(d, {}, "measured"), "measured"),
    ("capture-reference-dropped",
     None, lambda d: _drop(d, "capture", "reference"), "reference"),
    ("unknown-top-level-key",
     None, lambda d: _set(d, "precomputed", "source"), "source"),
    # --- rule 14: an ethos_u point's vela profile must not CONTRADICT what
    #     the module's own SoC spec declares (`npu_toolchain.vela`).  Rule 6
    #     already requires `memory_mode` to be PRESENT; this checks it
    #     against E1M-AEN801's host SoC spec's OWN declared value
    #     (metadata/socs/alif/ensemble/e8.json's `npu_toolchain.vela.
    #     memory_mode` is `Sram_Only`).  The mutant is placed at the path its
    #     OWN changed profile digest implies (recomputed via
    #     `V._toolchain_profile_digest`, never hand-copied), so rule 1's
    #     path-identity check stays silent and this case tests rule 14 alone.
    ("ethos-u-point-contradicts-the-socs-declared-memory-mode",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+"
     + V._toolchain_profile_digest({
         "name": "vela", "version": "5.1.0",
         "system_config": "Ethos_U85_SRAM_Only",
         "memory_mode": "Dedicated_Sram_384KB",
     }) + ".json",
     lambda d: _set(d, "Dedicated_Sram_384KB", "toolchain", "memory_mode"),
     "npu_toolchain.vela.memory_mode"),
    # --- rule 15: a SoC that requires the vendor config refuses one of
    #     vela's own Arm built-in System_Config names, even though rule 14
    #     stays silent -- the E8's npu_toolchain.vela declares no
    #     system_config VALUE of its own (only `memory_mode: "Sram_Only"`
    #     and `system_config_requires_vendor_config: true`), so rule 14 has
    #     nothing to compare `toolchain.system_config` against.  This
    #     mutant keeps `memory_mode` at the SoC spec's own correct
    #     `Sram_Only` (an Arm built-in memory_mode, reachable with no
    #     vendor `.ini`) so rule 14 stays quiet on both fields and only
    #     rule 15 fires.  `Ethos_U85_SYS_DRAM_Mid` is vela's own flagless
    #     default on the U85 (measured: `vela --accelerator-config
    #     ethos-u85-256 --memory-mode Sram_Only` with no `--config` prints
    #     `Warning: No system configuration specified. Using a default of
    #     Ethos_U85_SYS_DRAM_Mid.` and exits 0) -- the DRAM-backed machine
    #     this SRAM-only part is not.  The mutant's path is its OWN changed
    #     profile digest, same discipline as the rule-14 case above, so
    #     rule 1 stays silent too.
    ("ethos-u-point-records-velas-builtin-system-config-on-a-vendor-required-part",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/"
     "person-detect-int8-808cfdfc0cf3@vela-5.1.0+r2+m55_hp+"
     + V._toolchain_profile_digest({
         "name": "vela", "version": "5.1.0",
         "system_config": "Ethos_U85_SYS_DRAM_Mid",
         "memory_mode": "Sram_Only",
     }) + ".json",
     lambda d: _set(d, "Ethos_U85_SYS_DRAM_Mid", "toolchain", "system_config"),
     "vela's own Arm built-ins"),
]


#: The one mutation case that is ABOUT `_fixture`; every other case placed in
#: the published tree drops the marker first, so rule 12 does not fire as noise
#: on top of the rule actually under test.
_FIXTURE_MARKER_CASE = "fixture-marker-in-the-published-tree"


@pytest.fixture(scope="module")
def _gate_root(tmp_path_factory):
    """A scratch root carrying the real `metadata/socs/` and
    `metadata/e1m_modules/`, which the SKU / accel-config / hw_rev checks
    resolve against.  Copied rather than symlinked so this works on the
    Windows dev host too.  Nothing is ever written into the real checkout --
    `git stash` is not usable here (60+ worktrees share one .git)."""
    root = tmp_path_factory.mktemp("model_perf_gate")
    for sub in ("socs", "e1m_modules"):
        shutil.copytree(_ROOT / "metadata" / sub, root / "metadata" / sub)
    return root


@pytest.fixture(scope="module")
def gate(_gate_root):
    """Run the SHIPPED gate -- the real `model-perf-v1.schema.json` plus the
    real `validate_metadata._check_model_perf_semantics`, never a
    re-implementation -- over a document placed at an arbitrary repo-relative
    path.  Returns the gate's complaints, empty when it accepts the point."""
    validator = jsonschema.Draft202012Validator(_load(_SCHEMA_PATH))

    def _run(doc: dict, rel: str) -> list[str]:
        path = _gate_root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(doc, indent=2), encoding="utf-8")
        msgs = [f"{'/'.join(str(p) for p in e.absolute_path) or '<root>'}: {e.message}"
                for e in validator.iter_errors(doc)]
        with pytest.MonkeyPatch.context() as mp:
            mp.setattr(V, "REPO", _gate_root)
            for _rel, semantic in V._check_model_perf_semantics([path]):
                msgs.extend(semantic)
        return msgs

    return _run


def _at_its_own_correct_path(base: dict, rel: str) -> tuple[dict, str]:
    """The unmutated document as it would legitimately be committed: with the
    `_fixture` marker under `tests/fixtures/`, without it under
    `metadata/model_perf/`."""
    doc = copy.deepcopy(base)
    if rel.startswith("metadata/model_perf/"):
        doc.pop("_fixture", None)
    return doc, rel


def test_the_unmutated_fixture_passes_the_gate(gate):
    assert gate(_load(_BASE), _BASE_REL) == []


def test_the_same_point_passes_as_a_published_point_once_the_marker_is_gone(gate):
    """What a real campaign will commit: the identical document, without
    `_fixture`, under `metadata/model_perf/`.  This is the shape the rule-12
    mutant differs from by exactly one key."""
    doc, rel = _at_its_own_correct_path(_load(_BASE), _PUBLISHED_REL)
    assert gate(doc, rel) == []


@pytest.mark.parametrize("case_id,rel,mutate,expected", _MUTATIONS,
                         ids=[c[0] for c in _MUTATIONS])
def test_each_rule_bites(gate, case_id, rel, mutate, expected):
    """BOTH DIRECTIONS, every case.

    Green first: the unmutated document, at the path it would legitimately be
    committed to, must pass -- otherwise a mutant's "failure" could be the
    base document's doing rather than the mutation's, which is how a check
    that never bites looks exactly like one that does.  Then the mutant, at
    the path the case names, must fail with a complaint that names the rule.
    """
    green_doc, green_rel = _at_its_own_correct_path(_load(_BASE), _BASE_REL)
    assert gate(green_doc, green_rel) == [], (
        f"{case_id}: the unmutated document must pass before a mutant means "
        f"anything")

    mutant = copy.deepcopy(_load(_BASE))
    target_rel = rel or _BASE_REL
    if target_rel.startswith("metadata/model_perf/") and case_id != _FIXTURE_MARKER_CASE:
        mutant.pop("_fixture", None)
    if mutate is not None:
        mutant = mutate(mutant)
    msgs = gate(mutant, target_rel)
    assert msgs, f"{case_id}: the gate accepted a broken perf point"
    assert any(expected in m for m in msgs), (
        f"{case_id}: expected a complaint naming {expected!r}, got {msgs}")


# ---------------------------------------------------------------------------
# Rule 15 is keyed off the SoC spec's OWN `system_config_requires_vendor_
# config` flag, not off a hand-picked "known-bad" list -- two things the
# parametrized mutation above cannot prove on its own: (1) a point recording
# a genuine VENDOR system config on the part that requires one passes
# *because* it is outside vela's built-in set, not by accident; (2) the rule
# stays silent on a part that does NOT require one, even when that part's
# point records a name that happens to be one of vela's Arm built-ins.
# ---------------------------------------------------------------------------

def test_a_vendor_system_config_passes_on_the_part_that_requires_one(gate):
    """E1M-AEN801 / the E8 (`system_config_requires_vendor_config: true`,
    `metadata/socs/alif/ensemble/e8.json`). The shipped fixture already
    records `Ethos_U85_SRAM_Only` and already passes
    (`test_the_unmutated_fixture_passes_the_gate`); this pins down WHY --
    that name is not in vela's own built-in set, which is the actual thing
    rule 15 refuses."""
    doc = _load(_BASE)
    assert doc["toolchain"]["system_config"] == "Ethos_U85_SRAM_Only"
    assert "Ethos_U85_SRAM_Only" not in V._VELA_BUILTIN_SYSTEM_CONFIGS
    doc, rel = _at_its_own_correct_path(doc, _PUBLISHED_REL)
    assert gate(doc, rel) == []


def test_rule_15_does_not_fire_on_a_part_that_does_not_require_a_vendor_config(gate):
    """E1M-NX9101 / i.MX 93 declares `system_config_requires_vendor_config:
    false` (`metadata/socs/nxp/imx9/imx93.json`). A point recording one of
    vela's OWN Arm built-ins there -- `Ethos_U65_Mid_End`, chosen because it
    IS in `_VELA_BUILTIN_SYSTEM_CONFIGS` -- must still pass: rule 15 keys off
    the flag, and the flag is false, so the rule must not fire regardless of
    what the point's `system_config` says. `memory_mode` matches imx93's own
    declared `Shared_Sram` so rule 14 stays quiet too, isolating this proof
    to rule 15 -- i.e. no production SKU that legitimately has no vendor
    config is newly rejected by this change."""
    assert "Ethos_U65_Mid_End" in V._VELA_BUILTIN_SYSTEM_CONFIGS
    toolchain = {
        "name": "vela",
        "version": "5.1.0",
        "system_config": "Ethos_U65_Mid_End",
        "memory_mode": "Shared_Sram",
    }
    digest = V._toolchain_profile_digest(toolchain)
    stem = f"person-detect-int8-808cfdfc0cf3@vela-5.1.0+r1+a55_cluster+{digest}"
    doc = {
        "stance": "bench-measured",
        "measured_on": {
            "sku": "E1M-NX9101",
            "hw_rev": "r1",
            "core": "a55_cluster",
            "backend": "ethos_u",
            "accel_config": "ethos-u65-256",
        },
        "model": {
            "slug": "person-detect-int8",
            "sha256": "808cfdfc0cf3a6fa6f6fa26bfa379ea97c16d5db7334637766e39c3408502e9d",
            "size_bytes": 300568,
            "source": "tests/fixtures/models/person_detect_int8.tflite",
        },
        "toolchain": toolchain,
        "measured": {
            "npu_ops": 1,
            "cpu_ops": 0,
        },
        "capture": {
            "date": "2026-08-16",
            "operator": "test",
            "reference": "alp-sdk-internal:bench/captures/SYNTHETIC-imx93.log",
        },
    }
    rel = f"metadata/model_perf/E1M-NX9101/ethos-u65-256/{stem}.json"
    assert gate(doc, rel) == []
