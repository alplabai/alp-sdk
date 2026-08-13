#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Recipe-vs-west pin-parity gate (issue #1281).

Nothing cross-checked a `meta-alp-sdk` recipe's `SRCREV` against the
revision Zephyr's `west.yml` pins for the SAME third-party module --
`grep -rln SRCREV scripts/` returned no file before this gate.  A
`.alpmodel`-shaped library (zcbor: decoded by BOTH the M-class Zephyr
build and the A-class Yocto build) can drift to two different upstream
versions on the two OSes with CI staying green throughout, because
nothing compared the two pins.

Three sources exist for a curated third-party library (ADR 0018):

  1. the meta-alp-sdk recipe's own pin -- its `SRC_URI ...;tag=<X>`
     (the human-legible upstream tag) if present, else its literal
     `SRCREV = "<sha>"` (a raw 40-hex commit);
  2. `metadata/libraries/<name>.yaml`'s `version:` field, the recorded
     upstream pin;
  3. the WEST grounding for the same module -- either west.yml's own
     top-level `projects: [name: <module>]` `revision:` (when the
     module is pinned directly, not only through Zephyr's own
     `name-allowlist` import), or, for a Tier-B library Zephyr doesn't
     import at all, the manifest's own self-contained
     `integration.zephyr.west.revision` pin.

The recipe<->manifest link is BitBake's own naming convention, not an
invented field: a recipe file `<pn>_<pv>.bb` names a real third-party
library the moment `metadata/libraries/<pn>.yaml` exists -- exactly how
`zcbor_0.9.1.bb` (issue #1254 / PR #1280) names `zcbor`.

WHAT COUNTS AS A FAILURE VS. A DELIBERATE EXEMPTION (decided explicitly,
per the issue's own ask, not left implicit):

  * A recipe whose PN matches no `metadata/libraries/<pn>.yaml` (e.g.
    this layer's OWN `alp-sdk`/`alp-chips`/`dx-rt` recipes) is OUT OF
    SCOPE -- it isn't a curated third-party library pin at all, so
    there is no ground truth on the other side to compare against.
    Not a failure, not reported.
  * A recipe with NO `SRCREV` at all (a license-gated recipe with no
    fetchable source, e.g. `dx-rt_2.4.bb`), or one pinned to
    `${AUTOREV}`/any unresolved `${...}` expansion (a floating,
    non-reproducible pin -- ADR 0017 territory, a DIFFERENT gate's
    concern), has no literal value to compare.  Not a failure, not
    reported.
  * A library with no west grounding reachable from this gate -- either
    `integration.zephyr.module` is `null` (an in-tree Zephyr subsystem
    with no separate module, e.g. `lwm2m`/`coap`), or the module is
    reachable ONLY through Zephyr's own `name-allowlist` import (the
    exact zcbor shape: `west.yml` never states its revision directly --
    that lives in Zephyr's OWN manifest, resolvable only from a real
    checkout, the same network-heavy oracle
    `check_west_manifest_module_resolution.py` documents as its own
    "KNOWN LIMIT" and deliberately avoids) -- is a DELIBERATE EXEMPTION
    for the recipe<->west comparison specifically. The recipe<->version
    comparison (metadata/libraries's OWN recorded pin) still applies in
    this case, so a zcbor-shaped module is not left totally unchecked.
  * `west.yml` / `metadata/libraries/` / `meta-alp-sdk/` all missing, or
    a matched manifest's YAML failing to parse, IS a hard failure --
    these are required inputs this gate cannot do its job without, not
    optional per-recipe data.

WEST-AXIS COVERAGE, NAMED EXPLICITLY -- not left implicit.  As of this
tree, 18 of the 36 `metadata/libraries/*.yaml` manifests have a west
grounding this gate can compare a recipe against; the other 18 do not,
for one of three distinct reasons (re-verify against the live tree --
`metadata/libraries/*.yaml` gains entries; this count drifts):

  * IN-TREE / NO UPSTREAM GIT PIN ANYWHERE -- genuinely nothing to
    compare, not a gate gap.  `integration.zephyr.module: null` tied to
    Zephyr's OWN in-tree subsystem (the `zephyr` project's own
    revision, not a separate fetchable repo): coap, lwm2m, modbus.
    Maintainer-written / header-only, no upstream repo at all:
    gfx-compat, nlohmann-json, pid.  Yocto-only, no Zephyr side exists
    to ground against: ros2, onnxruntime.  (8 libraries)
  * REACHABLE ONLY THROUGH ZEPHYR'S OWN `name-allowlist` IMPORT --
    west.yml never states these modules' revision directly; only
    Zephyr's OWN manifest does, resolvable only from a real checkout,
    the same network-heavy oracle `check_west_manifest_module_resolution.py`
    documents as its own "KNOWN LIMIT" and deliberately avoids:
    cmsis-dsp, cmsis-nn, littlefs, lvgl, mbedtls, nanopb, tflite-micro,
    and -- the motivating case for this gate, #1254/#1280 -- ZCBOR.
    Their recipe<->metadata-version axis still runs (a real drift on
    that axis is still caught); only recipe<->west does not. (8
    libraries, zcbor included)
  * KNOWN GAP, NOT A DATA-UNAVAILABILITY EXEMPTION: arm-2d and cmsis-cv
    both correctly carry `module: null` (upstream ships no
    zephyr/module.yml for either, so neither is a Zephyr west MODULE),
    but west.yml DOES pin a real top-level PROJECT for each --
    `Arm-2D` at `v1.2.6`, `CMSIS-CV` at
    `25c6c111ee04dcfb0ae9093fd6dee4586872982c` -- kept there as the
    audit trail / version source for their Yocto recipes.
    `_west_grounding()` only reads `integration.zephyr.module`, so it
    never looks up a west.yml project pin filed under a different
    name; this is a real, fixable reach gap in this gate, not a stated
    exemption.  Left open here: closing it needs either a new schema
    field (a west-PROJECT name distinct from the Zephyr-MODULE name)
    or a name-matching heuristic, and neither has been reviewed -- a
    named follow-up, not silently folded into the "nothing to compare"
    bucket above. (2 libraries)

`test_west_axis_coverage_of_the_real_tree_is_named_here` in this gate's
own test file locks the 18/17 split and every name above to the real
tree, so this accounting cannot go stale without failing a test.

"SAME REVISION" VS. "SAME CONSUMED TREES" -- the deliberate answer the
issue asks this gate to state outright.  PR #1280 proved a real case
where two DIFFERENT SHAs (a zephyrproject-rtos mirror's mutable branch
tip vs. the canonical remote's tag) check out byte-identical `src`/
`include`/`LICENSE` subtrees -- a strict SHA-string equality check
would have flagged that as drift when it is provably fine.  Subtree-hash
comparison (clone both, hash the consumed paths) is the more honest
check for that case, and needs a real clone of two repos -- a
network-heavy oracle this gate deliberately does not carry, the same
tradeoff `check_west_manifest_module_resolution.py` already makes.
Instead: a recipe's ref and its comparison target are only compared
when they are the SAME KIND of reference -- both 40-hex git SHAs
(compared case-insensitively), or both NOT a SHA (a tag/branch string,
compared exactly).  A SHA-vs-tag pairing is a representation mismatch,
not provable drift without a real clone, so it is silently NOT
compared -- this is what keeps a zcbor-shaped pin (canonical tag vs.
mirror SHA, proven tree-identical by hand in the recipe's own comment)
from becoming a permanent false positive the moment `west.yml` ever
gains a resolvable top-level entry for it.

Run locally:

    python3 scripts/check_library_pin_parity.py

Exits non-zero if a recipe's pin and its metadata/libraries version, or
its recipe pin and its west grounding, disagree (same-kind pairs only).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

_SRCREV_RE = re.compile(r'(?m)^SRCREV(?::[A-Za-z0-9_.%-]+)?\s*=\s*"([^"]*)"')
# Captures the ASSIGNMENT OPERATOR as well as the value, because BitBake's
# `+=` APPENDS rather than replaces. Reading only the last assignment loses
# the pin on the common shape
#
#     SRC_URI = "git://...;tag=v1.2.3"
#     SRC_URI += " file://0001-fix.patch"
#
# where the last assignment is the patch line and carries no tag at all.
_SRC_URI_RE = re.compile(
    r'(?m)^SRC_URI(?::[A-Za-z0-9_.%-]+)?\s*(?P<op>\+?=)\s*"(?P<val>[^"]*)"')
_TAG_RE = re.compile(r';tag=([^\s;"\\]+)')
_SHA_RE = re.compile(r"[0-9a-fA-F]{40}")


def _looks_like_sha(ref: str) -> bool:
    return bool(_SHA_RE.fullmatch(ref))


def _mismatch(a: str, b: str) -> bool:
    """True only when `a` and `b` are the SAME KIND of git reference (both
    40-hex SHAs, or both not) and disagree.  A SHA paired with a
    non-SHA tag/branch string is a representation mismatch, not provable
    drift without a real clone -- see the module docstring's "SAME
    REVISION vs SAME CONSUMED TREES" section -- so it is never reported."""
    a_sha, b_sha = _looks_like_sha(a), _looks_like_sha(b)
    if a_sha != b_sha:
        return False
    return a.lower() != b.lower() if a_sha else a != b


def _recipe_ref(text: str) -> str | None:
    """The recipe's own pin: its SRC_URI `;tag=<X>` if present (the
    human-legible upstream tag), else its literal SRCREV.  `findall` +
    take-the-LAST match, not `re.search`'s first -- BitBake itself
    resolves a repeated unconditional assignment to whichever comes LAST
    in the file, so picking the first would compare against a value that
    was never actually live (the same first-match fail-open class issue
    #1264/#1265 already found and fixed).  The tag search is scoped to
    the actual last `SRC_URI = "..."` assignment's captured value, never
    the raw file text -- a `;tag=` sitting in a comment (e.g. a "previous
    pin was ...;tag=X" note) is not an assignment and must never be read
    as the live pin.  Checking for a tag does not require an SRCREV
    match to exist first: a recipe can be validly pinned via SRC_URI's
    tag alone, and gating the tag search behind "an SRCREV line was
    found" would silently exempt that recipe instead of reading its real
    pin -- only return None when NEITHER a tag NOR a literal SRCREV can
    be found at all."""
    # Fold the SRC_URI assignments the way BitBake does: `=` REPLACES what
    # came before, `+=` APPENDS to it.  Searching only the final assignment
    # loses the pin whenever a recipe appends a patch after setting its git
    # URL -- the last assignment is then ` file://0001-fix.patch`, which
    # carries no tag, and the recipe would be silently exempted from the
    # parity check instead of compared against its real pin.
    effective = ""
    for match in _SRC_URI_RE.finditer(text):
        if match.group("op") == "+=":
            effective += " " + match.group("val")
        else:
            effective = match.group("val")
    if effective:
        tag_match = _TAG_RE.search(effective)
        if tag_match:
            return tag_match.group(1)
    matches = _SRCREV_RE.findall(text)
    if not matches:
        return None  # no tag and no SRCREV -- e.g. a license-gated recipe with no fetchable source
    srcrev = matches[-1]
    if not srcrev or "${" in srcrev:
        return None  # ${AUTOREV} or any unresolved expansion -- not a literal, reproducible pin
    return srcrev


def _top_level_project_revisions(manifest: dict) -> dict[str, str]:
    out: dict[str, str] = {}
    for p in manifest.get("projects", []) or []:
        name, revision = p.get("name"), p.get("revision")
        if isinstance(name, str) and isinstance(revision, str):
            out[name] = revision
    return out


def _west_grounding(lib_doc: dict, rel_manifest: str,
                     top_level: dict[str, str]) -> tuple[str, str] | None:
    """(ref, source-label) for the same module's west grounding, or None
    when this module is a deliberate exemption (see module docstring)."""
    zephyr = (lib_doc.get("integration") or {}).get("zephyr")
    if not isinstance(zephyr, dict):
        return None
    west_block = zephyr.get("west")
    if isinstance(west_block, dict) and isinstance(west_block.get("revision"), str):
        return west_block["revision"], f"{rel_manifest} integration.zephyr.west.revision"
    module = zephyr.get("module")
    if isinstance(module, str) and module in top_level:
        return top_level[module], f"west.yml projects[name={module}].revision"
    return None  # module: null, or allowlist-only (no top-level west.yml entry) -- exempt


def find_problems(root: Path) -> list[str]:
    west_path = root / "west.yml"
    libdir = root / "metadata" / "libraries"
    meta_dir = root / "meta-alp-sdk"

    missing: list[str] = []
    if not west_path.is_file():
        missing.append("west.yml: not found -- cannot verify recipe/west pin parity")
    if not libdir.is_dir():
        missing.append("metadata/libraries: not found -- cannot verify recipe/west pin parity")
    if not meta_dir.is_dir():
        missing.append("meta-alp-sdk: not found -- cannot verify recipe/west pin parity")
    if missing:
        return missing

    try:
        manifest = yaml.safe_load(west_path.read_text(encoding="utf-8"))["manifest"]
    except (yaml.YAMLError, KeyError, TypeError) as e:
        return [f"west.yml: could not parse manifest -- {e}"]
    top_level = _top_level_project_revisions(manifest)

    problems: list[str] = []
    for bb in sorted(meta_dir.rglob("*.bb")):
        pn = bb.stem.partition("_")[0]
        manifest_path = libdir / f"{pn}.yaml"
        if not manifest_path.is_file():
            continue  # not a curated third-party library recipe -- out of scope

        recipe_ref = _recipe_ref(bb.read_text(encoding="utf-8", errors="replace"))
        if recipe_ref is None:
            continue  # no literal, reproducible pin on the recipe side -- nothing to compare

        try:
            lib_doc = yaml.safe_load(manifest_path.read_text(encoding="utf-8")) or {}
        except yaml.YAMLError as e:
            problems.append(f"{manifest_path.relative_to(root)}: could not parse -- {e}")
            continue

        rel_bb = bb.relative_to(root).as_posix()
        rel_manifest = manifest_path.relative_to(root).as_posix()

        version = lib_doc.get("version")
        if isinstance(version, str) and _mismatch(recipe_ref, version):
            problems.append(
                f"{rel_bb} pins {recipe_ref!r}, but {rel_manifest} declares "
                f"version {version!r} for the same library -- recipe and "
                f"recorded upstream version have drifted")

        grounding = _west_grounding(lib_doc, rel_manifest, top_level)
        if grounding is not None:
            west_ref, west_source = grounding
            if _mismatch(recipe_ref, west_ref):
                problems.append(
                    f"{rel_bb} pins {recipe_ref!r}, but {west_source} pins "
                    f"{west_ref!r} for the same module -- recipe and west "
                    f"have drifted")

    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print("check-library-pin-parity: a meta-alp-sdk recipe's pin has drifted "
              "from its west.yml / metadata/libraries counterpart:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(f"\ncheck-library-pin-parity: {len(problems)} problem(s) -- failing.",
              file=sys.stderr)
        return 1
    print("check-library-pin-parity: OK (every meta-alp-sdk recipe pin matches its "
          "metadata/libraries version and, where west.yml resolves one, its west "
          "grounding).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
