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
  * `TestMutations` drives the SHIPPED gate -- the real
    `metadata/schemas/model-perf-v1.schema.json` plus the real
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
_BASE = (_FIXTURES / "E1M-AEN801" / "ethos-u85-256"
         / "person-detect-int8-808cfdfc0cf3@vela-5.1.0.json")
_BASE_REL = "tests/fixtures/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json"

#: Where the same document would live if it were a real, published point.
#: Used to prove the published shape passes AND that `_fixture` is refused
#: there.
_PUBLISHED_REL = "metadata/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json"


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


@pytest.mark.parametrize("path", _ALL_POINTS, ids=_ids)
def test_an_in_repo_model_source_still_hashes_to_the_pinned_sha256(path):
    """Catches a fixture regenerated without re-benching: the point would keep
    quoting arena/latency figures for bytes that no longer exist.

    This check lives HERE and not in `validate_metadata.py` on purpose.
    `model.source` points into `tests/fixtures/`, which does not exist in the
    metadata-ONLY scratch clone `test_alp_cli_new_som.py::_clone_metadata_gates`
    runs that gate against, so a copy there could only ever be a silent skip --
    the same split, for the same reason, as `_check_soc_vela_memory_profile`'s
    `source` citations.  This suite always runs against the real checkout.
    """
    doc = _load(path)
    source = doc.get("model", {}).get("source")
    if not source:
        return
    target = _ROOT / source
    if not target.is_file():
        # NOT a skip: a `source` naming an in-repo path that is not there is
        # either a moved model or a typo, and both break traceability.
        assert "/" not in source and "\\" not in source, (
            f"{path.name}: model.source `{source}` looks like a repo-relative "
            f"path but no such file exists")
        return
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    assert digest == doc["model"]["sha256"], (
        f"{path.name}: model.source `{source}` now hashes to {digest}, but the "
        f"point was measured on {doc['model']['sha256']} -- the model changed "
        f"and the figures were not re-benched")
    assert target.stat().st_size == doc["model"]["size_bytes"], (
        f"{path.name}: model.size_bytes disagrees with `{source}` on disk")


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
     "metadata/model_perf/E1M-AEN601/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json",
     None, "measured_on.sku"),
    ("path-target-dir-disagrees",
     "metadata/model_perf/E1M-AEN801/ethos-u55-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json",
     None, "implies directory"),
    ("path-model-sha-disagrees",
     "metadata/model_perf/E1M-AEN801/ethos-u85-256/person-detect-int8-deadbeefcafe@vela-5.1.0.json",
     None, "imply filename"),
    ("path-toolchain-version-drifts-from-filename",
     None, lambda d: _set(d, "5.2.0", "toolchain", "version"), "imply filename"),
    ("path-toolchain-name-drifts-from-filename",
     None, lambda d: _set(d, "dxcom", "toolchain", "name"), "imply filename"),
    ("path-model-slug-drifts-from-filename",
     None, lambda d: _set(d, "person-detect", "model", "slug"), "imply filename"),
    # --- rule 2: the SKU exists --------------------------------------------
    ("sku-does-not-exist",
     "metadata/model_perf/E1M-AEN999/ethos-u85-256/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json",
     lambda d: _set(d, "E1M-AEN999", "measured_on", "sku"), "no SoM preset"),
    # --- rule 3: the SKU really has that target ----------------------------
    ("accel-config-the-sku-does-not-have",
     "metadata/model_perf/E1M-AEN801/ethos-u55-512/person-detect-int8-808cfdfc0cf3@vela-5.1.0.json",
     lambda d: _set(d, "ethos-u55-512", "measured_on", "accel_config"),
     "is not a target"),
    ("backend-the-sku-does-not-have",
     "metadata/model_perf/E1M-AEN801/deepx_dxm1/person-detect-int8-808cfdfc0cf3@dxcom-2.3.0.json",
     lambda d: _set(_set(_set(_set(d, "deepx_dxm1", "measured_on", "backend"),
                              "", "measured_on", "accel_config"),
                         "dxcom", "toolchain", "name"),
                    "2.3.0", "toolchain", "version"),
     "is not a target"),
    # --- rule 4: the hardware revision exists ------------------------------
    ("hw-rev-not-in-the-family-table",
     None, lambda d: _set(d, "r99", "measured_on", "hw_rev"),
     "is not a revision of the aen family"),
    # --- rule 5: an Ethos-U point records its vela profile ------------------
    ("ethos-u-point-without-system-config",
     None, lambda d: _drop(d, "toolchain", "system_config"),
     "records no system_config"),
    ("ethos-u-point-without-memory-mode",
     None, lambda d: _drop(d, "toolchain", "memory_mode"),
     "records no memory_mode"),
    # --- rule 6: p95 cannot undercut the mean ------------------------------
    ("p95-below-the-mean",
     None, lambda d: _set(d, 0.5, "measured", "latency_ms_p95"),
     "is below measured.latency_ms_mean"),
    # --- rule 7: the published tree cannot absorb a fixture -----------------
    ("fixture-marker-in-the-published-tree",
     _PUBLISHED_REL, None, "must never ship under metadata/model_perf/"),
    # --- rule 8: capture.reference cites a store, not a disk ----------------
    ("capture-reference-is-a-posix-local-path",
     None, lambda d: _set(d, "/home/someone/bench/2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    ("capture-reference-is-a-windows-local-path",
     None, lambda d: _set(d, "C:\\bench\\2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    ("capture-reference-is-a-onedrive-share",
     None, lambda d: _set(d, "OneDrive/bench/2026-08-16.log", "capture", "reference"),
     "path on one machine"),
    # --- the schema half of the same gate ----------------------------------
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
]


#: The one mutation case that is ABOUT `_fixture`; every other case placed in
#: the published tree drops the marker first, so rule 7 does not fire as noise
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
    `_fixture`, under `metadata/model_perf/`.  This is the shape the rule-7
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
