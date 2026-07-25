#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/bootstrap.json in lockstep with everything that copies it.

metadata/bootstrap.json (issue #917) is the single source of truth for the
Zephyr-workspace-assembly FACTS scripts/bootstrap.sh and scripts/bootstrap.ps1
both read today; tan (Rust, cross-platform) is the INTENDED future consumer
of the same facts, not a current reader. Without a drift gate the manifest is
just a copy of facts that already lived elsewhere. This check fails loudly
when:

  1. metadata/bootstrap.json doesn't validate against
     metadata/schemas/bootstrap-v1.schema.json.
  2. `zephyr.version` disagrees with the Zephyr `revision:` pinned under
     `- name: zephyr` in west.yml.
  3. scripts/bootstrap.sh or scripts/bootstrap.ps1 still hardcode the pinned
     Zephyr version (its full `vX.Y.Z` form or its `vX.Y` MAJOR.MINOR form --
     the exact shape both scripts' own workspace-reuse logic computes)
     outside a comment, instead of deriving it from the manifest at run time.
     A bash heredoc (`<<EOF` / `<<'EOF'` / `<<-EOF`) or PowerShell here-string
     (`@"..."@` / `@'...'@`) BODY line counts as code here even if it starts
     with '#' -- it is printed OUTPUT, not a source comment, so it cannot
     hide a literal the way an actual comment legitimately can (see
     `_iter_scannable_lines`).
  4. Any CI workflow's own Zephyr pin (the `--mr <ver>` west-init argument, or
     a `key:` cache line naming the Zephyr checkout itself -- NOT the
     separate Zephyr SDK toolchain cache, which tracks its own release)
     disagrees with `zephyr.version`.
  5. README.md's Zephyr badge disagrees with `zephyr.version`.
  6. `prerequisites.posix` disagrees with bootstrap.sh's hardcoded
     `REQUIRED_BINS=(...)`, `prerequisites.windows` disagrees with
     bootstrap.ps1's hardcoded `$Prereqs` name list, or
     `prerequisites.pythonMinVersion` disagrees with either script's
     hardcoded Python floor (bootstrap.sh's `PYTHON_MIN_VERSION="..."`,
     bootstrap.ps1's `-lt [version]"..."`). (These stay hardcoded in both
     scripts by design -- reading the manifest itself needs the very
     prerequisite it would be checking -- so this is what stops them from
     silently drifting instead.)
  7. Any manifest LEAF (recursing into every nested object except `env` and
     `nativeLibHints`, which are consumed as a whole group by a generic loop
     in both scripts, not by a per-field literal) isn't demonstrably read by
     at least one of the two scripts, and isn't in an explicit allowlist of
     leaves that are gate-asserted-instead (`prerequisites.*`, point 6 above)
     or purely structural (`_comment`) -- see `_check_no_orphaned_leaves`.
  8. `nativeLibHints`'s own group-level consumption bar is broken: the
     manifest's OS key set no longer matches bootstrap.sh's `for os_key in
     (...)` loop, or bootstrap.sh no longer references the "note"/"command"
     field names at all -- see `_check_native_lib_hints_consumption` and the
     `_GROUP_LEAF_PATHS` comment for why point 7's generic scan can't see
     this drift on its own (the OS key there is a runtime loop variable, not
     a literal string).
  9. metadata/bootstrap.json has a top-level key not listed in KNOWN_KEYS
     below, i.e. a new fact nobody has wired a check for yet. Runs even when
     the manifest ALSO fails schema validation (an unknown key trips
     `additionalProperties: false` too) so this guidance isn't hidden behind
     the bare schema error.

Run locally:

    python3 scripts/check_bootstrap_manifest.py
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "metadata" / "bootstrap.json"
SCHEMA = REPO / "metadata" / "schemas" / "bootstrap-v1.schema.json"
WEST_YML = REPO / "west.yml"
BOOTSTRAP_SH = REPO / "scripts" / "bootstrap.sh"
BOOTSTRAP_PS1 = REPO / "scripts" / "bootstrap.ps1"
README_MD = REPO / "README.md"

# Every CI workflow that assembles its own throwaway Zephyr workspace and
# therefore pins a Zephyr revision independent of west.yml/west update.
CI_WORKFLOWS = [
    REPO / ".github" / "workflows" / "pr-twister.yml",
    REPO / ".github" / "workflows" / "pr-tier-a-libraries.yml",
    REPO / ".github" / "workflows" / "nightly-aen-hil.yml",
    REPO / ".github" / "workflows" / "pr-getting-started-aen801.yml",
]

# Every top-level manifest key MUST be listed here -- main()'s unknown-key
# check below fails loudly if a key is added without anyone deciding how it
# gets policed (consumed by both scripts, or asserted equal to a hardcoded
# copy). This is the generalisation past just zephyr.version (issue #917).
KNOWN_KEYS = {
    "_comment", "schemaVersion", "zephyr", "venv", "prerequisites",
    "west", "pip", "env", "nativeLibHints", "manualInstallHints",
}

_WEST_MR_RE = re.compile(r"--mr\s+(v\d+\.\d+\.\d+)")
# A cache `key:` line that names the Zephyr checkout itself and embeds a
# literal version, e.g.
#   key: zephyr-v4.4.0-host-${{ runner.os }}
#   key: getting-started-aen801-zephyr-v4.4.0-${{ runner.os }}
# Requires "zephyr-v<digits>" CONTIGUOUS (no `-eabi-`/`-sdk-` etc. in
# between), which is exactly what excludes a Zephyr *SDK* toolchain cache
# key like `zephyr-sdk-arm-zephyr-eabi-v4.4.0-...` -- that key names the
# separate Zephyr SDK release (pinned independently, e.g. as
# `ZEPHYR_SDK_VERSION: "1.0.1"` elsewhere in the same workflow) and only
# agrees with `zephyr.version` today by coincidence, not by contract.
# Deliberately does NOT match a `${{ env.ZEPHYR_SDK_VERSION }}`-interpolated
# key either (no literal digits to compare).
_CACHE_KEY_RE = re.compile(r"key:.*?zephyr-(v\d+\.\d+\.\d+)", re.IGNORECASE)
_README_BADGE_RE = re.compile(r"Zephyr-v(\d+\.\d+\.\d+)")

# Leaves that stay hardcoded in both scripts BY DESIGN (see point 6 in the
# module docstring) -- policed by their own dedicated comparison checks
# below instead of the generic orphan-leaf scan.
_GATE_ASSERTED_LEAF_PREFIX = "prerequisites."
# Purely structural: the manifest's own self-description, never something a
# script "reads" as a fact. (`schemaVersion` is NOT here -- both scripts
# assert it at run time, see the orphan-leaf scan.)
_STRUCTURAL_LEAVES = {"_comment"}
# Leaves under these top-level keys are consumed as a WHOLE GROUP: both
# scripts loop over sub-keys generically (`d["env"].keys()`/`.values()`,
# `d["nativeLibHints"][os_key]`) rather than referencing one literal
# `d["env"]["ZEPHYR_BASE"]`-shaped needle per field, so recursion stops at
# the group name itself instead of descending into env.ZEPHYR_BASE etc.
#
# nativeLibHints specifically CANNOT be tightened to per-OS granularity
# (`nativeLibHints.linux`, `.macos`, `.windows`) the way an ordinary nested
# object could: bootstrap.sh's loop reads `d["nativeLibHints"][os_key]` with
# `os_key` a runtime loop variable, so no script ever contains a literal
# `d["nativeLibHints"]["windows"]` substring for a per-OS needle to find --
# tightening the generic scan that far would immediately misfire as a false
# orphan on the real repo. `_check_native_lib_hints_consumption` below is
# the deliberately separate, field-name-aware check that gives this group a
# real consumption bar instead (issue #917 review item 8): it asserts BOTH
# `hint["note"]` and `hint["command"]` are actually read, and that the
# os-key set a script iterates still matches the schema's, so a new
# nativeLibHints OS or field silently going unread doesn't stay green just
# because "nativeLibHints" as a bare string still appears somewhere.
_GROUP_LEAF_PATHS = {"env", "nativeLibHints"}


def _load_manifest_and_schema() -> tuple[dict, list[str]]:
    problems: list[str] = []
    if not MANIFEST.is_file():
        return {}, [f"missing {MANIFEST.relative_to(REPO).as_posix()}"]
    if not SCHEMA.is_file():
        return {}, [f"missing {SCHEMA.relative_to(REPO).as_posix()}"]
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(manifest), key=lambda e: list(e.absolute_path))
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        problems.append(f"schema: {loc}: {err.message}")
    return manifest, problems


def _check_west_yml(manifest_version: str) -> list[str]:
    if not WEST_YML.is_file():
        return [f"missing {WEST_YML.relative_to(REPO).as_posix()}"]
    text = WEST_YML.read_text(encoding="utf-8")
    m = re.search(r"-\s*name:\s*zephyr\s*\n\s*revision:\s*(\S+)", text)
    if m is None:
        return ["west.yml: could not find a `revision:` line under `- name: zephyr`"]
    west_version = m.group(1)
    if west_version != manifest_version:
        return [f"west.yml pins zephyr revision {west_version!r}, "
                 f"metadata/bootstrap.json declares zephyr.version {manifest_version!r}"]
    return []


def _hardcoded_literal_patterns(manifest_version: str) -> list[re.Pattern]:
    """Version literals a script must NOT hardcode outside a comment: the
    full pin (v4.4.0) and its MAJOR.MINOR form (v4.4 -- the exact shape both
    scripts' `PIN_MM` / `$PinMM` workspace-reuse logic computes from it).
    Each is anchored to the manifest's OWN version (not "any X.Y.Z-shaped
    string"), so unrelated three-component version strings elsewhere in a
    script (a `west>=0.14.0` pip floor, a `PowerShell 7+` remark, a Zephyr
    *SDK* release number) never trip this check; a leading 'v' is optional
    and a following digit is excluded so "4.4.0" doesn't also fire inside
    "4.4.01" and "4.4" doesn't double-fire inside its own "4.4.0"."""
    m = re.match(r"^v?(\d+)\.(\d+)\.(\d+)$", manifest_version)
    if not m:
        return []
    major, minor, patch = m.groups()
    full = re.compile(rf"v?{major}\.{minor}\.{patch}(?!\d)")
    major_minor = re.compile(rf"v?{major}\.{minor}(?!\.\d)")
    return [full, major_minor]


# bash heredoc opener: `<<EOF`, `<<'EOF'`, `<<"EOF"`, or `<<-EOF` (the `-`
# form strips LEADING TABS ONLY from both the body and the terminator line --
# not spaces; that's a real bash rule, not a simplification here). Captures
# the optional `-` and the (optionally quoted) terminator word.
_BASH_HEREDOC_START_RE = re.compile(r"<<(-)?\s*(['\"]?)(\w+)\2")
# PowerShell here-string opener: `@"` or `@'` as the LAST thing on the line
# (PowerShell itself requires nothing else follow it) -- captures which
# quote character was used so the matching closer (`"@` / `'@`) can be told
# apart from the other kind.
_PS1_HERESTRING_START_RE = re.compile(r"@(['\"])\s*$")


def _iter_scannable_lines(script_text: str):
    """Yield (line, is_comment) for every line of a bootstrap script,
    tracking bash heredoc (`<<EOF` / `<<'EOF'` / `<<-EOF`) and PowerShell
    here-string (`@"..."@` / `@'...'@`) state as it goes.

    Why this exists: a heredoc/here-string BODY line is printed OUTPUT, not
    source code -- treating a body line that happens to start with '#' as a
    genuine source comment (the naive `line.strip().startswith("#")` check
    this replaces) let a Zephyr-version literal printed verbatim to the user
    slip the hardcoded-literal gate entirely (issue #917 review item 2:
    rewriting a `# Run the local test suite:` OUTPUT line inside
    bootstrap.sh's `cat <<EOF` block to `# Zephyr v4.4.0 is required for
    this suite:` stayed green). Body lines are therefore always scanned
    (never treated as comments) regardless of a leading '#'; only lines
    OUTSIDE a heredoc/here-string still get the ordinary '#'-prefix comment
    skip.
    """
    in_heredoc = False
    heredoc_terminator = ""
    heredoc_strip_tabs = False
    in_herestring = False
    herestring_close = ""
    for line in script_text.splitlines():
        if in_heredoc:
            body_line = line.lstrip("\t") if heredoc_strip_tabs else line
            if body_line == heredoc_terminator:
                in_heredoc = False
            yield line, False
            continue
        if in_herestring:
            if line.startswith(herestring_close):
                in_herestring = False
            yield line, False
            continue
        # Not currently inside a heredoc/here-string body: this line is real
        # source (or the line that OPENS one), so the ordinary '#'-prefix
        # comment skip still applies to IT.
        m = _BASH_HEREDOC_START_RE.search(line)
        if m is not None:
            in_heredoc = True
            heredoc_strip_tabs = m.group(1) == "-"
            heredoc_terminator = m.group(3)
        else:
            m2 = _PS1_HERESTRING_START_RE.search(line.rstrip())
            if m2 is not None:
                in_herestring = True
                herestring_close = m2.group(1) + "@"
        yield line, line.strip().startswith("#")


def _check_no_hardcoded_literal(script: Path, manifest_version: str) -> list[str]:
    if not script.is_file():
        return [f"missing {script.relative_to(REPO).as_posix()}"]
    patterns = _hardcoded_literal_patterns(manifest_version)
    hits: set[str] = set()
    # Skip comment lines (both scripts use '#' comments) -- a rationale
    # comment is allowed to name the version it's explaining; only CODE (or
    # a heredoc/here-string BODY line, which is user-facing OUTPUT, not a
    # comment at all -- see _iter_scannable_lines) that should instead be
    # reading $ZEPHYR_VERSION / $ZephyrVersion is a problem.
    for line, is_comment in _iter_scannable_lines(script.read_text(encoding="utf-8")):
        if is_comment:
            continue
        for pat in patterns:
            hits.update(pat.findall(line))
    if hits:
        rel = script.relative_to(REPO).as_posix()
        return [f"{rel}: still hardcodes the pinned Zephyr version {sorted(hits)} outside "
                 f"a comment -- derive it from metadata/bootstrap.json instead"]
    return []


def _check_ci_workflow(path: Path, manifest_version: str) -> list[str]:
    if not path.is_file():
        return [f"missing {path.relative_to(REPO).as_posix()}"]
    text = path.read_text(encoding="utf-8")
    rel = path.relative_to(REPO).as_posix()
    found = _WEST_MR_RE.findall(text) + _CACHE_KEY_RE.findall(text)
    if not found:
        return [f"{rel}: no `--mr <ver>` west-init pin or zephyr cache `key:` line found "
                 f"-- update this gate if the pin format changed"]
    problems = []
    for version in found:
        if version != manifest_version:
            problems.append(f"{rel}: pins Zephyr {version!r}, "
                             f"metadata/bootstrap.json declares zephyr.version {manifest_version!r}")
    return problems


def _check_readme_badge(manifest_version: str) -> list[str]:
    if not README_MD.is_file():
        return [f"missing {README_MD.relative_to(REPO).as_posix()}"]
    text = README_MD.read_text(encoding="utf-8")
    m = _README_BADGE_RE.search(text)
    if m is None:
        return ["README.md: no `Zephyr-vX.Y.Z` badge found -- update this gate if the "
                 "badge format changed"]
    badge_version = f"v{m.group(1)}"
    if badge_version != manifest_version:
        return [f"README.md badge pins Zephyr {badge_version!r}, "
                 f"metadata/bootstrap.json declares zephyr.version {manifest_version!r}"]
    return []


# `prerequisites` is deliberately NOT read by either script at run time
# (reading the manifest needs python3, one of the very prerequisites being
# checked -- a bootstrap-of-the-bootstrap). Both scripts keep hardcoded
# copies instead; these checks are what stops those copies from drifting
# from metadata/bootstrap.json silently.

_SH_REQUIRED_BINS_RE = re.compile(r"REQUIRED_BINS=\(([^)]*)\)")
_SH_PYTHON_MIN_RE = re.compile(r'PYTHON_MIN_VERSION="([^"]+)"')
_PS1_PREREQ_NAME_RE = re.compile(r'Name\s*=\s*"([^"]+)"')
_PS1_PYTHON_MIN_RE = re.compile(r'-lt\s*\[version\]\s*"([^"]+)"')


def _check_prerequisites_posix(manifest: dict) -> list[str]:
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    m = _SH_REQUIRED_BINS_RE.search(text)
    if m is None:
        return ["scripts/bootstrap.sh: could not find `REQUIRED_BINS=(...)`"
                 " -- update this gate if it was renamed/restructured"]
    sh_bins = set(m.group(1).split())
    manifest_bins = set(manifest["prerequisites"]["posix"])
    if sh_bins != manifest_bins:
        return [f"scripts/bootstrap.sh REQUIRED_BINS={sorted(sh_bins)} disagrees with "
                 f"metadata/bootstrap.json prerequisites.posix={sorted(manifest_bins)}"]
    return []


def _check_prerequisites_windows(manifest: dict) -> list[str]:
    if not BOOTSTRAP_PS1.is_file():
        return [f"missing {BOOTSTRAP_PS1.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_PS1.read_text(encoding="utf-8")
    m = re.search(r"\$Prereqs\s*=\s*@\((.*?)\n\)", text, re.DOTALL)
    if m is None:
        return ["scripts/bootstrap.ps1: could not find `$Prereqs = @(...)`"
                 " -- update this gate if it was renamed/restructured"]
    ps1_bins = set(_PS1_PREREQ_NAME_RE.findall(m.group(1)))
    manifest_bins = set(manifest["prerequisites"]["windows"])
    if ps1_bins != manifest_bins:
        return [f"scripts/bootstrap.ps1 $Prereqs names={sorted(ps1_bins)} disagrees with "
                 f"metadata/bootstrap.json prerequisites.windows={sorted(manifest_bins)}"]
    return []


def _check_python_min_version_posix(manifest: dict) -> list[str]:
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    m = _SH_PYTHON_MIN_RE.search(text)
    if m is None:
        return ["scripts/bootstrap.sh: could not find `PYTHON_MIN_VERSION=\"...\"`"
                 " -- update this gate if it was restructured"]
    sh_floor = m.group(1)
    manifest_floor = manifest["prerequisites"]["pythonMinVersion"]
    if sh_floor != manifest_floor:
        return [f"scripts/bootstrap.sh hardcodes Python floor {sh_floor!r}, "
                 f"metadata/bootstrap.json declares prerequisites.pythonMinVersion {manifest_floor!r}"]
    return []


def _check_python_min_version_windows(manifest: dict) -> list[str]:
    if not BOOTSTRAP_PS1.is_file():
        return [f"missing {BOOTSTRAP_PS1.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_PS1.read_text(encoding="utf-8")
    m = _PS1_PYTHON_MIN_RE.search(text)
    if m is None:
        return ["scripts/bootstrap.ps1: could not find the `-lt [version]\"...\"` "
                 "Python floor check -- update this gate if it was restructured"]
    ps1_floor = m.group(1)
    manifest_floor = manifest["prerequisites"]["pythonMinVersion"]
    if ps1_floor != manifest_floor:
        return [f"scripts/bootstrap.ps1 hardcodes Python floor {ps1_floor!r}, "
                 f"metadata/bootstrap.json declares prerequisites.pythonMinVersion {manifest_floor!r}"]
    return []


def _iter_leaf_paths(obj, prefix: str = ""):
    """Yield every leaf (dotted-path) in the manifest, stopping recursion at
    `_GROUP_LEAF_PATHS` (consumed as a whole sub-tree, not field-by-field)."""
    if isinstance(obj, dict) and prefix not in _GROUP_LEAF_PATHS:
        for key, value in obj.items():
            child = f"{prefix}.{key}" if prefix else key
            yield from _iter_leaf_paths(value, child)
    else:
        yield prefix


def _bash_needle(leaf: str) -> str:
    return "d" + "".join(f'["{part}"]' for part in leaf.split("."))


def _ps1_needle(leaf: str) -> str:
    return "$Manifest." + leaf


def _check_no_orphaned_leaves(manifest: dict) -> list[str]:
    """Generalisation of "is this fact actually read by anything" past just
    `env`/`nativeLibHints` (issue #917 review finding: `west.pipSpec` shipped
    fully unread while the old, narrower check stayed green). A leaf need
    only be demonstrably read by AT LEAST ONE of the two scripts -- some
    facts are legitimately single-script (e.g. `venv.posixBinDir`, which
    bootstrap.ps1 has no reason to ever reference on native Windows)."""
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    if not BOOTSTRAP_PS1.is_file():
        return [f"missing {BOOTSTRAP_PS1.relative_to(REPO).as_posix()}"]
    sh_text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    ps1_text = BOOTSTRAP_PS1.read_text(encoding="utf-8")
    problems = []
    for leaf in _iter_leaf_paths(manifest):
        if leaf in _STRUCTURAL_LEAVES or leaf.startswith(_GATE_ASSERTED_LEAF_PREFIX):
            continue
        sh_needle = _bash_needle(leaf)
        ps1_needle = _ps1_needle(leaf)
        if sh_needle not in sh_text and ps1_needle not in ps1_text:
            problems.append(
                f"metadata/bootstrap.json leaf {leaf!r} is not read by scripts/bootstrap.sh "
                f"(expected {sh_needle!r}) or scripts/bootstrap.ps1 (expected {ps1_needle!r}) "
                f"-- wire it up, or add it to the allowlist in this gate with a reason"
            )
    return problems


def _check_native_lib_hints_consumption(manifest: dict) -> list[str]:
    """The dedicated, field-name-aware consumption bar for `nativeLibHints`
    described in the `_GROUP_LEAF_PATHS` comment above -- the generic
    per-leaf orphan scan cannot see past the group name here (the OS key is
    a runtime loop variable, not a literal string, in the script that reads
    it), so this check asserts the real thing that matters instead: the
    manifest's OS set is exactly what bootstrap.sh's loop iterates, and both
    the "note" and "command" field names are actually referenced (not just
    "nativeLibHints" as a bare, could-mean-anything substring)."""
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    problems = []
    manifest_os_keys = set(manifest["nativeLibHints"].keys())
    m = re.search(r"for\s+os_key\s+in\s+\(([^)]*)\)", text)
    if m is None:
        return ["scripts/bootstrap.sh: could not find the `for os_key in (...)` "
                 "nativeLibHints loop -- update this check if it was restructured"]
    script_os_keys = {tok.strip().strip("\"'") for tok in m.group(1).split(",") if tok.strip()}
    if script_os_keys != manifest_os_keys:
        problems.append(
            f"scripts/bootstrap.sh iterates nativeLibHints OS keys {sorted(script_os_keys)}, "
            f"metadata/bootstrap.json nativeLibHints declares {sorted(manifest_os_keys)}"
        )
    if 'hint["note"]' not in text:
        problems.append('scripts/bootstrap.sh no longer reads nativeLibHints.<os>.note '
                         '(expected `hint["note"]`) -- nativeLibHints.*.note would go unread')
    if 'hint["command"]' not in text:
        problems.append('scripts/bootstrap.sh no longer reads nativeLibHints.<os>.command '
                         '(expected `hint["command"]`) -- nativeLibHints.*.command would go unread')
    return problems


def _check_known_keys(manifest: dict) -> list[str]:
    unknown = set(manifest.keys()) - KNOWN_KEYS
    if unknown:
        return [f"metadata/bootstrap.json has key(s) {sorted(unknown)} not listed in "
                 f"KNOWN_KEYS in this gate -- add a check (consumed-by-at-least-one-script "
                 f"or asserted-equal-to-a-hardcoded-copy) and add the key to KNOWN_KEYS "
                 f"before merging"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    args = ap.parse_args()
    del args

    manifest, problems = _load_manifest_and_schema()
    if not manifest:
        # Nothing else to check -- the manifest or schema file is missing.
        print("FAIL metadata/bootstrap.json", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    # Run the known-top-level-keys check UNCONDITIONALLY, even if the schema
    # validation above already failed -- an unknown key trips
    # `additionalProperties: false` first, and main() used to return before
    # this ever ran, so its "add a check before merging" guidance never
    # printed alongside the bare schema error (issue #917 review finding).
    problems += _check_known_keys(manifest)
    if problems:
        print("FAIL metadata/bootstrap.json", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    manifest_version = manifest["zephyr"]["version"]

    problems = []
    problems += _check_west_yml(manifest_version)
    problems += _check_no_hardcoded_literal(BOOTSTRAP_SH, manifest_version)
    problems += _check_no_hardcoded_literal(BOOTSTRAP_PS1, manifest_version)
    problems += _check_readme_badge(manifest_version)
    problems += _check_prerequisites_posix(manifest)
    problems += _check_prerequisites_windows(manifest)
    problems += _check_python_min_version_posix(manifest)
    problems += _check_python_min_version_windows(manifest)
    problems += _check_no_orphaned_leaves(manifest)
    problems += _check_native_lib_hints_consumption(manifest)
    for wf in CI_WORKFLOWS:
        problems += _check_ci_workflow(wf, manifest_version)

    if problems:
        print(f"FAIL bootstrap manifest drift (metadata/bootstrap.json declares "
              f"zephyr.version {manifest_version!r}):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    print(f"check_bootstrap_manifest: OK -- metadata/bootstrap.json, west.yml, README.md, "
          f"scripts/bootstrap.sh, scripts/bootstrap.ps1, and {len(CI_WORKFLOWS)} "
          f"CI workflow(s) all agree on Zephyr {manifest_version}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
