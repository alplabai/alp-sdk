# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_library_pin_parity.py.

Issue #1281: a meta-alp-sdk recipe's SRCREV must not silently drift from
the west.yml revision / metadata/libraries version pinning the SAME
third-party library.  These tests lock down: a real drift on each of the
two comparison axes (recipe<->metadata version, recipe<->west.yml) fails
named; a missing required input (west.yml/metadata/libraries/meta-alp-sdk)
is a hard failure, never a silent `[]`; a repeated SRCREV assignment
resolves to the LAST one (matching BitBake's own semantics), not the
first `re.search()` would find; and three legitimate exemptions are never
flagged: a recipe with no metadata/libraries counterpart, a module
reachable only via Zephyr's own `name-allowlist` import (the exact zcbor
shape -- no top-level west.yml entry to compare against), and a
SHA-vs-tag representation mismatch (provably not comparable without a
real clone).

Run locally:

    python -m pytest tests/scripts/test_check_library_pin_parity.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_library_pin_parity as gate  # noqa: E402


def _west_yml(root: Path, extra_projects: str = "") -> None:
    (root / "west.yml").write_text(
        "manifest:\n"
        "  projects:\n"
        "    - name: cmsisstream\n"
        "      revision: v3.2.0\n"
        "      path: modules/lib/cmsis-stream\n"
        + extra_projects,
        encoding="utf-8",
    )


def _lib_manifest(root: Path, name: str, version: str, *,
                   module: str | None = "__unset__",
                   west_block: str | None = None) -> None:
    libdir = root / "metadata" / "libraries"
    libdir.mkdir(parents=True, exist_ok=True)
    lines = [
        "schema_version: 1",
        f"name: {name}",
        "tier: A",
        f'version: "{version}"',
        "license: Apache-2.0",
        "integration:",
        "  zephyr:",
    ]
    if west_block:
        lines.append("    west:")
        lines.append(west_block)
    elif module != "__unset__":
        lines.append(f"    module: {module if module is not None else 'null'}")
    else:
        lines.append("    module: null")
    (libdir / f"{name}.yaml").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _recipe(root: Path, pn: str, pv: str, srcrev: str, *, tag: str | None = None) -> None:
    recipes_dir = root / "meta-alp-sdk" / "recipes-devtools" / pn
    recipes_dir.mkdir(parents=True, exist_ok=True)
    src_uri = "git://example.com/upstream.git;protocol=https"
    if tag:
        src_uri += f";tag={tag}"
    (recipes_dir / f"{pn}_{pv}.bb").write_text(
        f'SRC_URI = "{src_uri}"\n'
        f'SRCREV  = "{srcrev}"\n',
        encoding="utf-8",
    )


_SHA_A = "9b07780aca6fb21f82a241ba386ad9b379809337"
_SHA_B = "9164bd18dcd88ff9d9ef98279501fc1093571017"


def _scaffold_matching_pair(root: Path) -> None:
    """cmsisstream: a recipe, a west.yml top-level project, and a
    metadata/libraries manifest, all in agreement -- the clean-tree case."""
    _west_yml(root)
    _lib_manifest(root, "cmsisstream", "v3.2.0", module="cmsisstream")
    _recipe(root, "cmsisstream", "3.2.0", _SHA_A, tag="v3.2.0")


def test_clean_tree_passes(tmp_path: Path):
    _scaffold_matching_pair(tmp_path)
    assert gate.find_problems(tmp_path) == []


def test_recipe_vs_west_yml_tag_drift_fails(tmp_path: Path):
    """The recipe's tag disagrees with west.yml's revision for the same
    module -- both are non-SHA tag strings (same kind), so this must be
    reported by name."""
    _scaffold_matching_pair(tmp_path)
    bb = tmp_path / "meta-alp-sdk" / "recipes-devtools" / "cmsisstream" / "cmsisstream_3.2.0.bb"
    bb.write_text(bb.read_text(encoding="utf-8").replace("tag=v3.2.0", "tag=v3.1.0"), encoding="utf-8")
    # metadata/libraries must also disagree with the OLD recipe tag for this
    # to isolate the recipe<->west axis only -- keep it in lockstep with the
    # (now-changed) recipe so only the west comparison fires.
    _lib_manifest(tmp_path, "cmsisstream", "v3.1.0", module="cmsisstream")
    problems = gate.find_problems(tmp_path)
    assert any("pins 'v3.1.0'" in p and "west.yml projects[name=cmsisstream].revision" in p
               and "'v3.2.0'" in p for p in problems), problems


def test_recipe_vs_metadata_version_drift_fails(tmp_path: Path):
    """The recipe's tag disagrees with metadata/libraries's recorded
    version -- reported by name, west.yml left untouched (so the west
    axis independently still passes)."""
    _scaffold_matching_pair(tmp_path)
    _lib_manifest(tmp_path, "cmsisstream", "v3.9.9", module="cmsisstream")
    problems = gate.find_problems(tmp_path)
    assert any("pins 'v3.2.0'" in p and "metadata/libraries/cmsisstream.yaml" in p
               and "'v3.9.9'" in p for p in problems), problems


def test_sha_srcrev_vs_sha_west_revision_drift_fails(tmp_path: Path):
    """Both sides are 40-hex SHAs (no tag= on the recipe) -- the SAME kind,
    so a real SHA mismatch is provable and must be reported."""
    _west_yml(tmp_path, extra_projects=(
        "    - name: cmsis-cv-like\n"
        f"      revision: {_SHA_B}\n"
        "      path: modules/lib/cmsis-cv-like\n"
    ))
    _lib_manifest(tmp_path, "cmsiscvlike", _SHA_B, module="cmsis-cv-like")
    _recipe(tmp_path, "cmsiscvlike", "0.0.0", _SHA_A)  # no tag= -- SRCREV is the ref
    problems = gate.find_problems(tmp_path)
    assert any(_SHA_A in p and _SHA_B in p and "west.yml" in p for p in problems), problems


def test_srcrev_case_insensitive_sha_match_not_flagged(tmp_path: Path):
    _west_yml(tmp_path, extra_projects=(
        "    - name: cmsis-cv-like\n"
        f"      revision: {_SHA_A.upper()}\n"
        "      path: modules/lib/cmsis-cv-like\n"
    ))
    _lib_manifest(tmp_path, "cmsiscvlike", _SHA_A, module="cmsis-cv-like")
    _recipe(tmp_path, "cmsiscvlike", "0.0.0", _SHA_A)
    assert gate.find_problems(tmp_path) == []


def test_repeated_srcrev_resolves_to_last_assignment_not_first(tmp_path: Path):
    """Two SRCREV assignments in one recipe -- BitBake itself resolves to
    the LAST unconditional one.  An accurate DECOY first, a drifted real
    value second: picking the first (a bare re.search()) would hide the
    real drift -- the exact fail-open class #1264/#1265 already fixed
    elsewhere in this gate family."""
    _scaffold_matching_pair(tmp_path)
    bb = tmp_path / "meta-alp-sdk" / "recipes-devtools" / "cmsisstream" / "cmsisstream_3.2.0.bb"
    bb.write_text(
        'SRC_URI = "git://example.com/upstream.git;protocol=https"\n'
        f'SRCREV  = "{_SHA_A}"\n'  # accurate-looking decoy, no tag= present here
        f'SRCREV  = "{_SHA_B}"\n',  # the real, live value BitBake actually uses
        encoding="utf-8",
    )
    _lib_manifest(tmp_path, "cmsisstream", _SHA_A, module="cmsisstream")
    problems = gate.find_problems(tmp_path)
    assert any(_SHA_B in p and _SHA_A in p for p in problems), problems


def test_tag_in_a_comment_is_not_read_as_the_recipe_pin(tmp_path: Path):
    """A `;tag=` sitting in a COMMENT line (e.g. a "previous pin was ..."
    note) is not an assignment and must never be read as the live pin --
    only the actual `SRC_URI = "..."` assignment's own value is searched
    for a tag.  Real SRCREV is SHA_A; metadata declares a genuinely
    drifted SHA_B -- both SHAs, same kind, must be reported.  Before the
    fix, an unanchored `_TAG_RE.search(text)` over the whole file matched
    the comment's decoy tag instead, turning this into a SHA-vs-tag
    "different kind" pair that the gate silently declines to compare."""
    _west_yml(tmp_path)
    _lib_manifest(tmp_path, "cmsisstream", _SHA_B, module="cmsisstream")
    recipes_dir = tmp_path / "meta-alp-sdk" / "recipes-devtools" / "cmsisstream"
    recipes_dir.mkdir(parents=True, exist_ok=True)
    (recipes_dir / "cmsisstream_3.2.0.bb").write_text(
        "# previous pin used ;tag=v9.9.9 before the vendor moved to SHA pins\n"
        'SRC_URI = "git://example.com/upstream.git;protocol=https"\n'
        f'SRCREV  = "{_SHA_A}"\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert any(_SHA_A in p and _SHA_B in p for p in problems), problems


def test_tag_only_recipe_with_no_srcrev_line_is_still_read(tmp_path: Path):
    """A recipe pinned ONLY via SRC_URI's `;tag=<X>`, with no `SRCREV =`
    line in the file at all, still has a real, extractable pin per the
    docstring's own stated priority (tag if present, else SRCREV) -- it
    must not be silently exempted just because no SRCREV line exists.
    Before the fix, `_recipe_ref` required an SRCREV match to exist
    before even looking for a tag, so this recipe returned None (treated
    as "nothing to compare") despite carrying a real, live tag pin."""
    _west_yml(tmp_path)
    _lib_manifest(tmp_path, "cmsisstream", "v9.9.9", module="cmsisstream")  # deliberately drifted
    recipes_dir = tmp_path / "meta-alp-sdk" / "recipes-devtools" / "cmsisstream"
    recipes_dir.mkdir(parents=True, exist_ok=True)
    (recipes_dir / "cmsisstream_3.2.0.bb").write_text(
        'SRC_URI = "git://example.com/upstream.git;protocol=https;tag=v3.2.0"\n',
        encoding="utf-8",
    )
    problems = gate.find_problems(tmp_path)
    assert any("v3.2.0" in p and "v9.9.9" in p for p in problems), problems


def test_recipe_with_no_metadata_libraries_counterpart_not_flagged(tmp_path: Path):
    """A recipe whose PN names no metadata/libraries/<pn>.yaml (this
    layer's own product recipes, e.g. dx-rt/alp-sdk) is out of scope --
    there is no ground truth on the other side to compare against."""
    _west_yml(tmp_path)
    (tmp_path / "metadata" / "libraries").mkdir(parents=True, exist_ok=True)
    _recipe(tmp_path, "dx-rt", "2.4.0", "0" * 40)
    assert gate.find_problems(tmp_path) == []


def test_autorev_recipe_not_flagged(tmp_path: Path):
    """${AUTOREV} is not a literal, reproducible pin -- nothing to compare
    (a different gate's floating-pin concern, not this one)."""
    _west_yml(tmp_path)
    _lib_manifest(tmp_path, "cmsisstream", "v3.2.0", module="cmsisstream")
    recipes_dir = tmp_path / "meta-alp-sdk" / "recipes-devtools" / "cmsisstream"
    recipes_dir.mkdir(parents=True, exist_ok=True)
    (recipes_dir / "cmsisstream_0.6.bb").write_text(
        'SRC_URI = "git://example.com/upstream.git;protocol=https"\n'
        'SRCREV  = "${AUTOREV}"\n',
        encoding="utf-8",
    )
    assert gate.find_problems(tmp_path) == []


def test_module_reachable_only_via_zephyr_allowlist_exempts_west_axis(tmp_path: Path):
    """The exact zcbor shape (issue #1254/#1280): the module has NO
    top-level west.yml project (only reachable via Zephyr's own
    name-allowlist import, unresolvable without a real Zephyr checkout).
    The recipe<->west comparison is a deliberate exemption -- but the
    recipe<->metadata-version comparison still applies and still catches
    a real drift on that axis."""
    (tmp_path / "west.yml").write_text(
        "manifest:\n"
        "  projects:\n"
        "    - name: zephyr\n"
        "      revision: v4.4.1\n"
        "      import:\n"
        "        name-allowlist:\n"
        "          - zcbor\n",
        encoding="utf-8",
    )
    _lib_manifest(tmp_path, "zcbor", "0.9.1", module="zcbor")
    _recipe(tmp_path, "zcbor", "0.9.1", _SHA_A, tag="0.9.1")
    assert gate.find_problems(tmp_path) == []  # everything agrees -- clean

    # Now drift ONLY the metadata version -- must still be caught even
    # though the west axis is exempt for this module.
    _lib_manifest(tmp_path, "zcbor", "0.9.2", module="zcbor")
    problems = gate.find_problems(tmp_path)
    assert any("pins '0.9.1'" in p and "metadata/libraries/zcbor.yaml" in p
               and "'0.9.2'" in p for p in problems), problems
    assert not any("west.yml" in p for p in problems), problems


def test_sha_vs_tag_representation_mismatch_not_flagged(tmp_path: Path):
    """PR #1280's real shape: the recipe pins a canonical-remote TAG; the
    comparison target (here, metadata's OWN recorded version) is a raw
    SHA -- proven tree-identical by a human, but not provable by this
    gate without a real clone.  Different KINDS of ref must never be
    compared -- see the module docstring's SAME REVISION vs SAME
    CONSUMED TREES section."""
    _west_yml(tmp_path)
    _lib_manifest(tmp_path, "cmsisstream", _SHA_B, module="cmsisstream")  # a raw SHA, not a tag
    _recipe(tmp_path, "cmsisstream", "3.2.0", _SHA_A, tag="v3.2.0")  # recipe ref is the TAG
    assert gate.find_problems(tmp_path) == []


def test_west_block_self_pin_used_when_present(tmp_path: Path):
    """A Tier-B library with its OWN integration.zephyr.west.revision
    self-pin (aws-iot/azure-iot shape) is compared against that pin, not
    a top-level west.yml project (there is none for this module)."""
    _west_yml(tmp_path)
    _lib_manifest(tmp_path, "some-sdk", "v3.1.5",
                   west_block='      name: some-sdk\n'
                              '      url: https://github.com/example/some-sdk.git\n'
                              '      revision: v3.1.5\n'
                              '      path: modules/lib/some-sdk\n')
    _recipe(tmp_path, "some-sdk", "3.1.5", _SHA_A, tag="v3.1.5")
    assert gate.find_problems(tmp_path) == []

    _lib_manifest(tmp_path, "some-sdk", "v3.1.5",
                   west_block='      name: some-sdk\n'
                              '      url: https://github.com/example/some-sdk.git\n'
                              '      revision: v3.1.6\n'
                              '      path: modules/lib/some-sdk\n')
    problems = gate.find_problems(tmp_path)
    assert any("pins 'v3.1.5'" in p and "integration.zephyr.west.revision" in p
               and "'v3.1.6'" in p for p in problems), problems


def test_missing_west_yml_fails_loudly_not_silently(tmp_path: Path):
    (tmp_path / "metadata" / "libraries").mkdir(parents=True, exist_ok=True)
    (tmp_path / "meta-alp-sdk").mkdir(parents=True, exist_ok=True)
    problems = gate.find_problems(tmp_path)
    assert any("west.yml" in p and "not found" in p for p in problems)


def test_missing_metadata_libraries_fails_loudly_not_silently(tmp_path: Path):
    _west_yml(tmp_path)
    (tmp_path / "meta-alp-sdk").mkdir(parents=True, exist_ok=True)
    problems = gate.find_problems(tmp_path)
    assert any("metadata/libraries" in p and "not found" in p for p in problems)


def test_missing_meta_alp_sdk_fails_loudly_not_silently(tmp_path: Path):
    _west_yml(tmp_path)
    (tmp_path / "metadata" / "libraries").mkdir(parents=True, exist_ok=True)
    problems = gate.find_problems(tmp_path)
    assert any("meta-alp-sdk" in p and "not found" in p for p in problems)


# Issue #1281's own headline ask ("cross-check the west axis") is inert for
# most of metadata/libraries/ -- this locks the module docstring's WEST-AXIS
# COVERAGE accounting to the REAL tree, so the enumeration cannot drift
# silently out of date: a library added, removed, or reclassified without
# updating both the docstring and this test fails here first.
_WEST_AXIS_IN_TREE_OR_NO_UPSTREAM_PIN = {
    "coap", "lwm2m", "modbus", "gfx-compat", "nlohmann-json", "pid", "ros2",
    # Yocto-only, like ros2: pinned by its own meta-alp-sdk recipe (upstream
    # microsoft/onnxruntime v1.28.0), with no Zephyr side and no west.yml
    # project to ground a revision against.
    "onnxruntime",
}
_WEST_AXIS_ALLOWLIST_ONLY = {
    "cmsis-dsp", "cmsis-nn", "littlefs", "lvgl", "mbedtls", "nanopb",
    "tflite-micro", "zcbor",
}
_WEST_AXIS_KNOWN_GAP = {"arm-2d", "cmsis-cv"}


def test_west_axis_coverage_of_the_real_tree_is_named_here():
    """Re-derives, against the REAL repo's west.yml + metadata/libraries/
    (not a tmp_path fixture), which manifests `_west_grounding()` can
    ground and which it cannot -- and asserts the "cannot" set is exactly
    the three named categories the module docstring enumerates.  Proves
    zcbor -- the issue's own motivating case -- is named as an exemption
    rather than silently uncovered."""
    manifest = yaml.safe_load((REPO / "west.yml").read_text(encoding="utf-8"))["manifest"]
    top_level = gate._top_level_project_revisions(manifest)

    libdir = REPO / "metadata" / "libraries"
    covered: set[str] = set()
    not_covered: set[str] = set()
    for f in sorted(libdir.glob("*.yaml")):
        lib_doc = yaml.safe_load(f.read_text(encoding="utf-8")) or {}
        grounding = gate._west_grounding(lib_doc, f.name, top_level)
        (covered if grounding is not None else not_covered).add(f.stem)

    expected_not_covered = (_WEST_AXIS_IN_TREE_OR_NO_UPSTREAM_PIN
                             | _WEST_AXIS_ALLOWLIST_ONLY | _WEST_AXIS_KNOWN_GAP)
    assert not_covered == expected_not_covered, (
        f"west-axis coverage drifted from the docstring's accounting -- "
        f"newly uncovered: {not_covered - expected_not_covered}, "
        f"newly covered: {expected_not_covered - not_covered}")
    assert "zcbor" in not_covered  # the issue's own motivating case, named not silent

    # NOT `covered | not_covered == {f.stem for f in libdir.glob("*.yaml")}`:
    # both sets are populated by iterating that exact glob and adding
    # `f.stem`, so their union equals the glob BY CONSTRUCTION and the
    # assertion can never fail.  Check the two things that can:
    #
    #   1. the sweep really ran over a corpus -- an empty or collapsed glob
    #      would make every assertion above pass vacuously;
    #   2. the west axis grounds a real share of it.  If `covered` ever
    #      empties, the axis is inert everywhere and the gate degrades to
    #      its SRCREV-only half while still reporting green.
    assert len(covered) + len(not_covered) >= 30, (
        "metadata/libraries/ glob collapsed -- the assertions above would "
        "pass vacuously over an empty corpus")
    assert not covered & not_covered, "a manifest cannot be both covered and not covered"
    assert len(covered) >= 15, (
        f"the west axis grounds only {len(covered)} manifests -- it was 18 when "
        f"this accounting was written, and an axis grounding nothing is a gate "
        f"reporting green while checking only one side")


def test_src_uri_append_does_not_lose_the_tag():
    """BitBake's `+=` APPENDS, so reading only the LAST SRC_URI assignment
    loses the pin on the commonest real shape: a git URL carrying the tag,
    followed by an appended patch line.  Before the assignments were folded
    the way BitBake folds them, `_recipe_ref` returned None here -- a
    fail-open that silently exempted the recipe from the parity check
    instead of comparing its real pin."""
    text = (
        'SRC_URI = "git://x.example/r.git;protocol=https;branch=main;tag=v1.2.3"\n'
        'SRC_URI += " file://0001-fix.patch"\n'
    )
    assert gate._recipe_ref(text) == "v1.2.3"


def test_plain_reassignment_still_takes_the_last_value():
    """`=` REPLACES.  The append fold must not turn a later plain
    assignment into an accumulation, or an intentionally re-pinned recipe
    would report its superseded tag."""
    text = (
        'SRC_URI = "git://x.example/r.git;tag=vOLD"\n'
        'SRC_URI = "git://x.example/r.git;tag=vNEW"\n'
    )
    assert gate._recipe_ref(text) == "vNEW"
