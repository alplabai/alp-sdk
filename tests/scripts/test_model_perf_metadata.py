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
