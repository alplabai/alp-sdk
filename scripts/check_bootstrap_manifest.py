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
     separate Zephyr SDK toolchain cache, which tracks its own release and
     is instead gated by `scripts/check_toolchain_lock.py` against
     `metadata/toolchains.json`, issue #949 item 3)
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
     The same walk ALSO asserts a leaf being read is not ALSO sitting beside
     a hardcoded duplicate of itself: any whitespace-delimited fragment of a
     string leaf's value that is >= `_DUPLICATE_LITERAL_MIN_LEN` characters
     must not appear as a CODE-line literal (outside a comment, by the same
     heredoc/here-string-aware `_iter_scannable_lines` point 3 uses) in
     either script (issue #965 -- `scripts/bootstrap.ps1` printed
     `manualInstallHints.windows.note`'s Arm-toolchain installer URL both as
     a rendered manifest read AND as a hardcoded here-string for as long as
     that duplicate existed, and the plain "is this read by something" scan
     above cannot distinguish that from a single correct read). KNOWN LIMIT:
     the fragment-length floor means a duplicated short leaf value (under
     `_DUPLICATE_LITERAL_MIN_LEN` chars) is not caught this way -- see that
     constant's own comment for why a length floor was chosen over a
     hardcoded exemption list, and `prerequisites.*`/`_comment` are excluded
     from this scan the same as from the read-scan above (the former is
     gate-asserted equal on purpose by point 6's own checks, so a repeat
     there is by design, not drift). `env` and `nativeLibHints` are ALSO
     exempt from this scan, for the same reason point 8 gives them their own
     dedicated group-consumption check instead of the generic per-leaf walk:
     `_iter_leaf_paths` stops recursion at those two group names, so no
     per-field leaf (e.g. `env.ZEPHYR_BASE`) is ever produced for either
     assertion to see.
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
  10. A metadata/libraries/*.yaml manifest's `version:` field disagrees with
      `zephyr.version` when that manifest is a genuine IN-TREE ZEPHYR
      SUBSYSTEM -- per metadata/schemas/library-v1.schema.json:31, its
      pinned upstream version IS the pinned Zephyr release. Derived
      structurally every run (never a hardcoded filename list):
      `integration.zephyr.module: null` (no separate west module) alone is
      NOT enough -- alp-sdk's own in-tree source (e.g. `pid`, `gfx-compat`)
      and unpinned placeholders (e.g. `nlohmann-json`) also carry
      `module: null` but pin their OWN version, not Zephyr's. The
      OS-exclusivity a real in-tree Zephyr subsystem also always declares
      (`requires.os == ["zephyr"]` -- it exists only inside the zephyr repo,
      nothing else) is what actually distinguishes the two; see
      `_in_tree_zephyr_library_manifests`.
  11. `prerequisites.install` (issue #949) -- the single source for every
      per-tool install COMMAND (as opposed to point 6 above, which only
      polices the tool NAME lists) -- disagrees with anything that copies
      one of those commands: a tool listed in `prerequisites.windows` or
      `prerequisites.posix` with no matching `install.<os>.<tool>` entry;
      `scripts/bootstrap.ps1`'s own hardcoded `$Prereqs` `Hint = "..."`
      value for a tool disagreeing with `install.windows.<tool>`; or one of
      `install.windows`'s own winget PACKAGE IDs (derived from the manifest,
      never hardcoded a second time) appearing in the scanned doc/script
      file set without its full canonical command alongside it -- or one
      of those PACKAGE IDs failing to extract from its own `install.windows`
      command in the first place. The literal scan covers the windows/
      winget side only; see `_check_install_commands`'s own docstring for
      why, the full assertion list, and the exact file set scanned.

--fix propagates a changed `zephyr.version` OUT to every machine pin site
this gate verifies above (points 2, 4, 5, 10 -- west.yml, the CI workflow
`--mr`/cache-key pins, the README badge, and every in-tree-Zephyr-subsystem
library manifest's `version:` field). It reuses the exact same
compiled regexes/constants the verify-only checks read (`_WEST_YML_ZEPHYR_RE`,
`_WEST_MR_RE`, `_CACHE_KEY_RE`, `_README_BADGE_RE`, `_LIBRARY_VERSION_RE`) --
there is deliberately no second, parallel pin map; that would just be a new
flavour of the drift issue #917 exists to kill. Idempotent (a site already
at zephyr.version is left untouched, byte-for-byte); a site the gate expects
but can no longer find/match is a hard failure naming it, never a silent
no-op. bootstrap.sh and bootstrap.ps1 are NOT --fix sites -- they read
zephyr.version from the manifest at run time and must never hardcode it
(that's what point 3 above polices); prose docs, CHANGELOG history, and each
library manifest's own `# Grounding (pinned Zephyr ...)` provenance comment
are out of scope for a mechanical regex rewrite by design (see
docs/zephyr-version-policy.md) -- only the `version:` field itself is a fix
site.

Run locally:

    python3 scripts/check_bootstrap_manifest.py
    python3 scripts/check_bootstrap_manifest.py --fix
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import jsonschema
import yaml

REPO = Path(__file__).resolve().parent.parent
MANIFEST = REPO / "metadata" / "bootstrap.json"
SCHEMA = REPO / "metadata" / "schemas" / "bootstrap-v1.schema.json"
WEST_YML = REPO / "west.yml"
BOOTSTRAP_SH = REPO / "scripts" / "bootstrap.sh"
BOOTSTRAP_PS1 = REPO / "scripts" / "bootstrap.ps1"
README_MD = REPO / "README.md"
LIBRARIES_DIR = REPO / "metadata" / "libraries"

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

# The `revision:` line under `- name: zephyr` in west.yml. Hoisted to a
# module constant (rather than staying inline in `_check_west_yml` as it did
# before --fix existed) specifically so `_run_fix` reuses this SAME compiled
# pattern instead of a second, drift-prone copy of the same shape.
_WEST_YML_ZEPHYR_RE = re.compile(r"-\s*name:\s*zephyr\s*\n\s*revision:\s*(\S+)")

_LIBRARY_VERSION_RE = re.compile(r'^version:\s*"([^"]*)"', re.MULTILINE)
_WEST_MR_RE = re.compile(r"--mr\s+(v\d+\.\d+\.\d+)")
# A cache `key:` line that names the Zephyr checkout itself and embeds a
# literal version, e.g.
#   key: zephyr-v4.4.0-host-${{ runner.os }}
#   key: getting-started-aen801-zephyr-v4.4.0-${{ runner.os }}
# Requires "zephyr-v<digits>" CONTIGUOUS (no `-eabi-`/`-sdk-` etc. in
# between), which is exactly what excludes a Zephyr *SDK* toolchain cache
# key like `zephyr-sdk-arm-zephyr-eabi-${{ env.ZEPHYR_SDK_VERSION }}-...` --
# that key names the separate Zephyr SDK release (pinned independently in
# `metadata/toolchains.json`, gated by `scripts/check_toolchain_lock.py`,
# not this script) and would only agree with `zephyr.version` today by
# coincidence, not by contract.  Deliberately does NOT match a
# `${{ env.ZEPHYR_SDK_VERSION }}`-interpolated key either (no literal
# digits to compare).
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
    m = _WEST_YML_ZEPHYR_RE.search(text)
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


def _in_tree_zephyr_library_manifests() -> list[Path]:
    """metadata/libraries/*.yaml manifests that are genuine IN-TREE ZEPHYR
    SUBSYSTEMS -- the only per-library manifests whose `version:` must equal
    the pinned Zephyr release itself (module docstring point 10).

    `integration.zephyr.module: null` alone is NOT the bar: it also covers
    alp-sdk's own maintainer-written in-tree source (`pid`, `gfx-compat`,
    each pinning its OWN version/SHA) and unpinned best-effort placeholders
    (`nlohmann-json`). What actually distinguishes a real in-tree Zephyr
    subsystem -- and is what every one of coap/lwm2m/modbus's own header
    comments says in prose -- is that it exists ONLY inside the zephyr repo
    and nowhere else, which every one of them also encodes structurally as
    `requires.os == ["zephyr"]`. alp-sdk's own in-tree libraries and
    placeholders never declare that OS-exclusivity (they're portable, or
    the constraint hasn't been written down), so requiring BOTH conditions
    -- not `module: null` alone -- is what keeps this derived set exactly
    right without a hardcoded filename list.
    """
    if not LIBRARIES_DIR.is_dir():
        return []
    result = []
    for path in sorted(LIBRARIES_DIR.glob("*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        zephyr = (doc.get("integration") or {}).get("zephyr")
        if not isinstance(zephyr, dict) or "module" not in zephyr or zephyr["module"] is not None:
            continue
        os_list = (doc.get("requires") or {}).get("os") or []
        if set(os_list) == {"zephyr"}:
            result.append(path)
    return result


def _check_library_versions(manifest_version: str) -> list[str]:
    target = manifest_version.removeprefix("v")
    problems = []
    for path in _in_tree_zephyr_library_manifests():
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        lib_version = doc.get("version")
        rel = path.relative_to(REPO).as_posix()
        if lib_version != target:
            problems.append(
                f"{rel}: version {lib_version!r} disagrees with metadata/bootstrap.json "
                f"zephyr.version {manifest_version!r} -- this manifest is an in-tree "
                f"Zephyr subsystem (module: null, requires.os == ['zephyr']), so its "
                f"pinned upstream version IS the pinned Zephyr release"
            )
    return problems


def _apply_version_fix(path: Path, pattern: re.Pattern, target: str, *,
                        site_desc: str) -> tuple[list[str], list[str]]:
    """Rewrite every occurrence of `pattern`'s captured group 1 in `path` to
    `target`, editing ONLY the captured substring -- everything else in the
    file (surrounding text, line endings) is left exactly as-is, no reflow.
    Reused by every --fix site below so there is one rewrite mechanism, not
    one per site.

    Returns (change-report lines, problems). `pattern` matching nothing at
    all in `path` is a problem (`site_desc` names it) -- a regex that stops
    matching must fail loudly here, never silently write nothing. A pattern
    that matches but is already == target is NOT a problem and produces no
    report line (idempotency: a clean second run touches nothing).

    Writes with `newline=""` deliberately -- these files are pinned LF via
    .gitattributes (`eol=lf`), and Path.write_text's default newline
    translation rewrites '\\n' to os.linesep on write, which on a Windows
    host would silently flip every line in the file to CRLF (the same class
    of Windows-only-digest trap `alp.lock`'s `_dir_digest` hit -- see
    docs/zephyr-version-policy.md and the CHANGELOG entry for this change).

    A write that fails (e.g. the file went read-only mid-sweep) is caught
    and reported as a problem naming the file, never left to propagate as a
    bare uncaught exception -- an uncaught exception here would abort the
    whole --fix sweep, silently leaving every site after this one in the
    caller's loop unrewritten with no report of what did or didn't land.
    """
    if not path.is_file():
        return [], [f"missing {path.relative_to(REPO).as_posix()}"]
    rel = path.relative_to(REPO).as_posix()
    text = path.read_text(encoding="utf-8")
    matches = list(pattern.finditer(text))
    if not matches:
        return [], [f"{rel}: {site_desc} not found -- --fix has nothing to rewrite; "
                     f"update this gate if the site's format changed"]
    report: list[str] = []
    new_text = text
    # Walk matches back-to-front: rewriting the rightmost match first means
    # every span still to be processed (all to its left) stays valid even
    # when `target` is a different LENGTH than what it replaces (e.g. a
    # v4.4.0 -> v4.10.0 bump), no separate offset bookkeeping needed.
    for m in reversed(matches):
        old = m.group(1)
        if old == target:
            continue
        start, end = m.span(1)
        line_no = new_text.count("\n", 0, start) + 1
        new_text = new_text[:start] + target + new_text[end:]
        report.append(f"{rel}:{line_no}: {old} -> {target}")
    if report:
        try:
            path.write_text(new_text, encoding="utf-8", newline="")
        except OSError as exc:
            return [], [f"{rel}: could not write {site_desc} -- {exc}"]
    return list(reversed(report)), []


def _fix_ci_workflow(path: Path, manifest_version: str) -> tuple[list[str], list[str]]:
    """A CI workflow may carry the `--mr` pin, the cache `key:` pin, or
    both (see CI_WORKFLOWS' docstring table) -- fix whichever it actually
    has, same as `_check_ci_workflow` verifies whichever it actually has."""
    if not path.is_file():
        return [], [f"missing {path.relative_to(REPO).as_posix()}"]
    text = path.read_text(encoding="utf-8")
    if not (_WEST_MR_RE.search(text) or _CACHE_KEY_RE.search(text)):
        rel = path.relative_to(REPO).as_posix()
        return [], [f"{rel}: no `--mr <ver>` west-init pin or zephyr cache `key:` line found "
                     f"-- update this gate if the pin format changed"]
    report: list[str] = []
    problems: list[str] = []
    for pattern, desc in (
        (_WEST_MR_RE, "the `--mr <ver>` west-init pin"),
        (_CACHE_KEY_RE, "the zephyr cache `key:` line"),
    ):
        # Re-read so the second pattern sees any rewrite the first just made
        # to the same file.
        if pattern.search(path.read_text(encoding="utf-8")):
            r, p = _apply_version_fix(path, pattern, manifest_version, site_desc=desc)
            report += r
            problems += p
    return report, problems


def _run_fix(manifest_version: str) -> int:
    """--fix entry point: rewrite west.yml, every CI_WORKFLOWS pin, and the
    README badge FROM manifest_version, then report what changed."""
    report: list[str] = []
    problems: list[str] = []

    r, p = _apply_version_fix(WEST_YML, _WEST_YML_ZEPHYR_RE, manifest_version,
                               site_desc="the `revision:` line under `- name: zephyr`")
    report += r
    problems += p

    for wf in CI_WORKFLOWS:
        r, p = _fix_ci_workflow(wf, manifest_version)
        report += r
        problems += p

    # _README_BADGE_RE's own capture group excludes the leading 'v'
    # (`Zephyr-v(\d+\.\d+\.\d+)`) -- target it with the same de-'v'd shape,
    # not manifest_version verbatim, or an already-correct badge would look
    # perpetually "different" and get rewritten (with a 'v' duplicated) on
    # every single run, breaking idempotency.
    r, p = _apply_version_fix(README_MD, _README_BADGE_RE, manifest_version.removeprefix("v"),
                               site_desc="the `Zephyr-vX.Y.Z` badge")
    report += r
    problems += p

    target = manifest_version.removeprefix("v")
    for lib_path in _in_tree_zephyr_library_manifests():
        r, p = _apply_version_fix(lib_path, _LIBRARY_VERSION_RE, target,
                                   site_desc="the `version:` field")
        report += r
        problems += p

    # Print whatever DID get rewritten first, unconditionally -- a failure
    # partway through the sweep above (a site that failed to match, or a
    # write that raised OSError) must never hide that other sites already
    # landed on disk (module docstring point about --fix's failure mode).
    if report:
        for line in report:
            print(line)

    if problems:
        print(f"FAIL check_bootstrap_manifest --fix (metadata/bootstrap.json declares "
              f"zephyr.version {manifest_version!r}):", file=sys.stderr)
        for prob in problems:
            print(f"  · {prob}", file=sys.stderr)
        return 1

    if report:
        print(f"check_bootstrap_manifest --fix: rewrote {len(report)} site(s) to Zephyr "
              f"{manifest_version}.")
    else:
        print(f"check_bootstrap_manifest --fix: every site already agrees with Zephyr "
              f"{manifest_version} -- nothing to do.")
    return 0


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


# -------- prerequisites.install (issue #949) ---------------------------------
#
# Unlike the NAME lists above (which stay hardcoded in both scripts BY
# DESIGN -- see the section comment above _SH_REQUIRED_BINS_RE), the per-tool
# install COMMAND has no such bootstrap-of-the-bootstrap circularity anywhere
# it duplicates: bootstrap.ps1's own $Prereqs already carries a Hint= string
# for the same reason (printing "install this" before python/JSON is even
# confirmed present), but every OTHER site that ever needs a copy of that
# string -- a doc, `alp doctor` (which runs from INSIDE the workspace venv,
# so it has no such circularity and could always have read the manifest) --
# has no excuse to hardcode its own. `install.windows` is compared against
# bootstrap.ps1's Hint= values below because bootstrap.ps1 is still the
# AUTHORITY for that value (it's the one script that cannot read this file
# for it); every other site is checked by the repo-wide literal scan instead.

# `Name = "git";    Hint = "winget install -e --id Git.Git"` -- both fields
# from the SAME $Prereqs entry, so a Hint can be compared against the
# matching tool's install.windows entry (as opposed to _PS1_PREREQ_NAME_RE
# above, which only ever extracted Name= for the point-6 tool-set check).
_PS1_PREREQ_ENTRY_RE = re.compile(r'Name\s*=\s*"([^"]+)"\s*;\s*Hint\s*=\s*"([^"]+)"')

# Pulls the winget PACKAGE ID out of one `install.windows.<tool>` command
# string ("winget install -e --id Git.Git" -> "Git.Git"). Used by
# `_winget_ids_and_commands` below to derive the literal scan's trigger set
# FROM the manifest at run time -- see that function's own docstring for why
# this must never be a second hardcoded copy of the four IDs.
_WINGET_ID_RE = re.compile(r"--id\s+(\S+)")


def _winget_ids_and_commands(install_windows: dict) -> tuple[dict[str, str], list[str]]:
    """({winget PackageId: its full canonical command string}, problems),
    derived from `prerequisites.install.windows` at run time.

    This -- not a hardcoded verb list like "winget install" -- is the
    literal scan's trigger set (issue #949 review). A bare verb fires on
    every install one-liner in the repo (Arm toolchain casks, Yocto apt
    walkthroughs, tutorial package hints, ...), none of which duplicate a
    manifest fact; the thing that actually marks a line as a COPY of a
    tracked command is the distinctive package identifier the manifest
    itself declares (`Git.Git`, `Kitware.CMake`, `Python.Python.3.12`,
    `Ninja-build.Ninja` today) appearing without its canonical command
    around it. Deriving the ID set from the manifest instead of
    hardcoding a second copy here is the same discipline this whole
    change is enforcing everywhere else -- a hardcoded copy in the gate
    that's supposed to catch hardcoded copies would be self-defeating.

    A command `_WINGET_ID_RE` doesn't match is itself reported as a
    problem, not silently excluded from the trigger set: the whole scan
    exists to catch a copy of a tracked command going stale elsewhere in
    the repo, so a tracked command with no extractable ID would make the
    scan cover nothing for that tool repo-wide while every one of the
    three assertions in `_check_install_commands` stayed green -- the
    reviewer's exact reproduction for this defect.
    """
    ids: dict[str, str] = {}
    problems: list[str] = []
    for tool, cmd in install_windows.items():
        m = _WINGET_ID_RE.search(cmd)
        if m:
            ids[m.group(1)] = cmd
        else:
            problems.append(
                f"prerequisites.install.windows.{tool} = {cmd!r} has no "
                f"`--id <PackageId>` for the literal scan to key on -- the "
                f"repo-wide scan cannot cover this tool until the command is "
                f"fixed to the `winget install -e --id <PackageId>` shape"
            )
    return ids, problems


# Historical/decision-record doc trees that legitimately quote an exact
# identifier as evidence about a PAST or PROPOSED state rather than
# asserting a current one -- the same reasoning check_doc_drift.py already
# applies to its own identifier scan. docs/adr/0021-toolchain-provisioning.md
# is the concrete, present-day case: it quotes `Git.Git` / `Kitware.CMake` /
# `Python.Python.3.12` / `Ninja-build.Ninja` while describing that tan-cli's
# `steps.rs` (a SEPARATE repo) hardcodes them too -- true, cited as context
# for a proposal, not an alp-sdk install command to reconcile. Only `adr` is
# excluded (YAGNI): nothing under docs/superpowers/** or docs/abi/** quotes a
# tracked identifier today -- add either only once a real case shows up.
_LITERAL_SCAN_EXCLUDE_DOC_DIRS = ("adr",)


def _iter_literal_scan_files():
    """Yield every path the winget-identifier literal scan
    (`_check_install_commands` point 3, issue #949) reads -- deliberately
    narrow (not the whole tree) so the gate stays cheap and its file set is
    easy to reason about: `docs/**/*.md` (excluding
    `_LITERAL_SCAN_EXCLUDE_DOC_DIRS`), `scripts/**/*.py` (recursive),
    `scripts/*.ps1` and `scripts/*.sh` (top-level only -- the only
    bootstrap-shaped scripts in this repo live directly under scripts/;
    firmware/**'s and meta-alp-sdk/**'s own shell scripts are a different
    concern, vendor/board bring-up rather than host bootstrap, and are out
    of scope), and README.md."""
    docs_dir = REPO / "docs"
    if docs_dir.is_dir():
        for path in sorted(docs_dir.rglob("*.md")):
            if path.relative_to(docs_dir).parts[0] in _LITERAL_SCAN_EXCLUDE_DOC_DIRS:
                continue
            yield path
    scripts_dir = REPO / "scripts"
    if scripts_dir.is_dir():
        yield from sorted(scripts_dir.rglob("*.py"))
        yield from sorted(scripts_dir.glob("*.ps1"))
        yield from sorted(scripts_dir.glob("*.sh"))
    if README_MD.is_file():
        yield README_MD


def _check_install_commands(manifest: dict) -> list[str]:
    """Drift gate for `prerequisites.install` (issue #949) -- the single
    source every per-tool install COMMAND must agree with. Three
    independent assertions, each covering a different slice:

      1. Completeness -- `install.windows`'s keys equal `prerequisites.
         windows`'s tools, and `install.linux` / `install.macos`'s keys
         each equal `prerequisites.posix`'s tools. A tool with no install
         command is the exact hole that produced the drifted/incomplete
         ninja hint in scripts/alp_cli/doctor.py this change fixes. This
         is the ONLY assertion covering install.linux / install.macos --
         see point 3's own note on why.
      2. scripts/bootstrap.ps1 agreement -- each `$Prereqs` entry's
         `Hint = "..."` value must equal `install.windows[<Name>]`
         byte-for-byte. Asserted HERE, not by extending
         `_check_prerequisites_windows` above (which only ever parsed
         `Name=` for the point-6 tool-SET check) -- this is a new,
         separate assertion over a field that function never looked at.
      3. Repo-wide literal scan, WINDOWS SIDE ONLY -- walks
         `_iter_literal_scan_files()` looking for one of the winget
         PACKAGE IDs `_winget_ids_and_commands` derives from
         `install.windows` (`Git.Git`, `Kitware.CMake`,
         `Python.Python.3.12`, `Ninja-build.Ninja` today -- never
         hardcoded a second time here). A line containing an ID without
         its full canonical command string is a drifted copy -- this is
         exactly the shape scripts/alp_cli/doctor.py's old ninja hint
         had (`winget install Ninja-build.Ninja.` contains the ID but not
         `winget install -e --id Ninja-build.Ninja`).

         KNOWN LIMITATION: the match is PER LINE, so a canonical command
         legitimately wrapped across two source lines (e.g. a PowerShell
         string literal split `"winget install -e --id "` + `"Ninja-build.
         Ninja"`) false-positives -- there is no escape hatch for that
         shape short of a trailing `#` comment on the line carrying the ID.
         Not fixed here: multi-line-joining the scan would need to track
         string-continuation syntax per language (bash, PowerShell, Python,
         Markdown all differ), which is a bigger change than this gate's
         narrow per-line design set out to be.

         KNOWN LIMITATION: the trigger set is derived from install.windows's
         CURRENT values (deliberately -- see _winget_ids_and_commands's own
         docstring for why that must stay a derivation, not a second
         hardcoded copy), so a LOCKSTEP RENAME -- install.windows.<tool> and
         bootstrap.ps1's Hint= both changed together to a new package ID --
         orphans every existing copy of the OLD id silently: the scan only
         ever catches a drifted copy of the id the manifest currently
         declares, never one the manifest used to declare and no longer
         does. Not fixed here for the same reason as the wrapped-string
         case above.

         Deliberately does NOT scan for linux/macos install commands --
         posix coverage instead stops at completeness (point 1); see
         `_winget_ids_and_commands`'s own docstring for why a verb-triggered
         scan was rejected in favour of keying on the identifier.

         `scripts/bootstrap.ps1` reuses `_iter_scannable_lines` (the same
         heredoc/here-string-aware comment tracking `_check_no_hardcoded_
         literal` already uses) so a rationale comment naming an ID stays
         exempt the same way it does there; `.py` files only skip a plain
         `#`-prefixed line as a comment, and `.md` files have no comment
         syntax at all, so every line counts there. This gate script's own
         source (which necessarily quotes the ID regex) is excluded by
         name, not by content -- it would otherwise flag itself.
    """
    problems: list[str] = []
    prereqs = manifest.get("prerequisites", {})
    install = prereqs.get("install", {})

    # -------- 1. completeness ---------------------------------------------
    windows_tools = set(prereqs.get("windows", []))
    windows_install = set(install.get("windows", {}))
    if windows_install != windows_tools:
        problems.append(
            f"prerequisites.install.windows keys {sorted(windows_install)} disagree "
            f"with prerequisites.windows tools {sorted(windows_tools)} -- every "
            f"windows prerequisite needs its own install command"
        )
    posix_tools = set(prereqs.get("posix", []))
    for os_key in ("linux", "macos"):
        os_install = set(install.get(os_key, {}))
        if os_install != posix_tools:
            problems.append(
                f"prerequisites.install.{os_key} keys {sorted(os_install)} disagree "
                f"with prerequisites.posix tools {sorted(posix_tools)} -- every "
                f"posix prerequisite needs its own install command on {os_key}"
            )

    # -------- 2. scripts/bootstrap.ps1 Hint= agreement ---------------------
    if not BOOTSTRAP_PS1.is_file():
        problems.append(f"missing {BOOTSTRAP_PS1.relative_to(REPO).as_posix()}")
    else:
        ps1_text = BOOTSTRAP_PS1.read_text(encoding="utf-8")
        m = re.search(r"\$Prereqs\s*=\s*@\((.*?)\n\)", ps1_text, re.DOTALL)
        if m is None:
            problems.append(
                "scripts/bootstrap.ps1: could not find `$Prereqs = @(...)` -- "
                "update this gate if it was renamed/restructured"
            )
        else:
            entries = _PS1_PREREQ_ENTRY_RE.findall(m.group(1))
            if not entries:
                problems.append(
                    "scripts/bootstrap.ps1: `$Prereqs` entries have no "
                    "`Hint = \"...\"` field -- update this gate if the shape changed"
                )
            windows_install_map = install.get("windows", {})
            # A PARTIAL parse -- an entry `_PS1_PREREQ_ENTRY_RE` couldn't
            # match (its `Hint = "..."` field was deleted, or `Name=`/`Hint=`
            # appear in some other order than the `Name = "..."; Hint =
            # "..."` shape that regex requires) -- silently drops that tool
            # out of `entries` while `if not entries:` above only fires when
            # EVERY entry fails to parse. That's the same "goes dark" defect
            # `_winget_ids_and_commands` above was fixed for, one level
            # down: a tool present in install.windows with no entry it can
            # be checked against must be named, not silently skipped by the
            # loop below.
            parsed_names = {name for name, _ in entries}
            for tool in sorted(windows_install_map):
                if tool not in parsed_names:
                    problems.append(
                        f"scripts/bootstrap.ps1 $Prereqs has no parseable "
                        f"`Name = \"{tool}\"; Hint = \"...\"` entry for "
                        f"prerequisites.install.windows.{tool} -- its Hint= "
                        f"field is missing, or Name=/Hint= appear out of "
                        f"order, so this gate cannot verify it agrees"
                    )
            for name, hint in entries:
                canonical = windows_install_map.get(name)
                if canonical is None:
                    problems.append(
                        f"scripts/bootstrap.ps1 $Prereqs entry {name!r} has "
                        f"Hint={hint!r}, but metadata/bootstrap.json has no "
                        f"prerequisites.install.windows.{name}"
                    )
                elif hint != canonical:
                    problems.append(
                        f"scripts/bootstrap.ps1 $Prereqs entry {name!r} Hint={hint!r} "
                        f"disagrees with prerequisites.install.windows.{name}={canonical!r}"
                    )

    # -------- 3. winget-identifier literal scan (windows side only) --------
    winget_ids, winget_id_problems = _winget_ids_and_commands(install.get("windows", {}))
    problems.extend(winget_id_problems)
    for path in _iter_literal_scan_files():
        rel = path.relative_to(REPO).as_posix()
        if rel == "scripts/check_bootstrap_manifest.py":
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if path.suffix in (".ps1", ".sh"):
            scannable = _iter_scannable_lines(text)
        elif path.suffix == ".py":
            scannable = ((line, line.strip().startswith("#")) for line in text.splitlines())
        else:
            # .md (and anything else in the file set): every line counts --
            # a Markdown `#`/`##`/... heading is not a comment, and treating
            # it as one let a heading naming a drifted winget command slip
            # the scan entirely (issue #949 review).
            scannable = ((line, False) for line in text.splitlines())
        for lineno, (line, is_comment) in enumerate(scannable, start=1):
            if is_comment:
                continue
            for pkg_id, canonical in winget_ids.items():
                if pkg_id in line and canonical not in line:
                    problems.append(
                        f"{rel}:{lineno}: winget package id {pkg_id!r} found without its "
                        f"canonical command {canonical!r} -- {line.strip()!r}"
                    )
    return problems


def _iter_leaf_paths(obj, prefix: str = ""):
    """Yield every (dotted-path, value) leaf pair in the manifest, stopping
    recursion at `_GROUP_LEAF_PATHS` (consumed as a whole sub-tree, not
    field-by-field). Yielding the value alongside its path -- rather than
    making a caller re-walk the same path with a separate lookup helper --
    is deliberate: the only consumer (`_check_no_orphaned_leaves`) needs
    both together, and a second walk of the same tree is pure duplication."""
    if isinstance(obj, dict) and prefix not in _GROUP_LEAF_PATHS:
        for key, value in obj.items():
            child = f"{prefix}.{key}" if prefix else key
            yield from _iter_leaf_paths(value, child)
    else:
        yield prefix, obj


def _bash_needle(leaf: str) -> str:
    return "d" + "".join(f'["{part}"]' for part in leaf.split("."))


def _ps1_needle(leaf: str) -> str:
    return "$Manifest." + leaf


# Minimum length, in characters, for a whitespace-delimited fragment of a
# string leaf's value to be treated as "distinctive" enough to police for a
# hardcoded duplicate (issue #965, `_check_no_orphaned_leaves`'s second
# assertion below). A short generic fragment (`bin`, `Scripts`, `zephyr`,
# `.venv`, even `alp-migrate`, all < 20 chars) is exactly the kind of weak
# trigger issue #965 warns against: it would need a growing allowlist of
# incidental matches (a rationale comment that happens to name the same
# short word for an unrelated reason) to stay usable. A length floor is a
# narrower rule that needs none: every leaf value in the manifest today
# that is genuinely short-and-generic stays under it, while the fragment
# that shipped duplicated for as long as it did -- the Arm GNU Toolchain
# installer URL in `manualInstallHints.windows.note`, 65 characters -- and
# every other today's-manifest fragment worth policing (long paths, long
# URLs, long sentences-as-a-whole) clears it easily. Swept against the real
# repo (see the test suite) with zero false positives at this value.
_DUPLICATE_LITERAL_MIN_LEN = 20

# Characters trimmed off both ends of an extracted fragment before the
# length check above and the literal search itself -- sentence punctuation
# (a trailing '.', a wrapping '(...)') that legitimately surrounds a fact in
# prose but is never part of a genuine hardcoded COPY of it (a script that
# duplicates a URL copies the URL, not the parenthesis around it in the
# manifest's sentence).
_DUPLICATE_LITERAL_STRIP_CHARS = "()[]{}.,;:'\""

_DUPLICATE_LITERAL_TOKEN_RE = re.compile(r"\S+")


def _distinctive_literals(value) -> set[str]:
    """Extract every whitespace-delimited fragment of `value` (a string, or
    a list of strings -- `manualInstallHints.*.note`/`nativeLibHints.*.note`
    shape) that is >= `_DUPLICATE_LITERAL_MIN_LEN` characters after
    stripping `_DUPLICATE_LITERAL_STRIP_CHARS` from both ends. Fragment, not
    the value as a WHOLE: a duplicate can reword the sentence around a fact
    while keeping the fact itself verbatim (issue #965's reproduction kept
    the installer URL but reworded the surrounding text), so matching only
    the complete leaf string would miss it. Non-string list elements (there
    are none in the schema today) are skipped rather than raising."""
    texts = value if isinstance(value, list) else [value]
    out: set[str] = set()
    for text in texts:
        if not isinstance(text, str):
            continue
        for tok in _DUPLICATE_LITERAL_TOKEN_RE.findall(text):
            tok = tok.strip(_DUPLICATE_LITERAL_STRIP_CHARS)
            if len(tok) >= _DUPLICATE_LITERAL_MIN_LEN:
                out.add(tok)
    return out


def _check_no_orphaned_leaves(manifest: dict) -> list[str]:
    """Generalisation of "is this fact actually read by anything" past just
    `env`/`nativeLibHints` (issue #917 review finding: `west.pipSpec` shipped
    fully unread while the old, narrower check stayed green). A leaf need
    only be demonstrably read by AT LEAST ONE of the two scripts -- some
    facts are legitimately single-script (e.g. `venv.posixBinDir`, which
    bootstrap.ps1 has no reason to ever reference on native Windows).

    A second, independent assertion per leaf (issue #965): "is read by"
    alone cannot tell a single correct read apart from a correct read
    sitting beside a hardcoded DUPLICATE of the same fact -- exactly the
    shape `scripts/bootstrap.ps1` shipped in for as long as it printed
    `manualInstallHints.windows.note`'s Arm-toolchain installer URL both
    from a rendered manifest read and from a hardcoded here-string, with a
    comment between them claiming the opposite. For every distinctive
    fragment `_distinctive_literals` extracts from a leaf's value, both
    scripts are scanned (via `_iter_scannable_lines`, so a heredoc/
    here-string BODY line -- printed OUTPUT, not source comment -- counts
    the same way it does for point 3's Zephyr-version scan) for that
    fragment appearing on a CODE line. It legitimately can never appear
    there at all: both scripts load `metadata/bootstrap.json` at run time
    (`json.load`/`ConvertFrom-Json`) and read a leaf's value through that
    parsed object, never by spelling the value out as a literal in their own
    source -- so any occurrence outside a comment is, by construction, a
    second copy, not a second legitimate reference.

    KNOWN LIMIT: a duplicated leaf value shorter than
    `_DUPLICATE_LITERAL_MIN_LEN` is not caught by this second assertion (see
    that constant's own comment for why a length floor was chosen over a
    hardcoded exemption list) -- it still has to clear the first assertion
    above (demonstrably read by at least one script), which a hardcoded
    duplicate does not by itself defeat. KNOWN LIMIT, separately: neither
    assertion here ever sees a leaf under `env` or `nativeLibHints` at all --
    `_iter_leaf_paths` stops recursion at those two group names (see
    `_GROUP_LEAF_PATHS`), so a duplicate of, say, `env.ZEPHYR_BASE` is
    outside this check's reach; `_check_native_lib_hints_consumption` gives
    `nativeLibHints` its own dedicated consumption bar, but no equivalent
    duplicate-literal scan exists for either group today.
    """
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    if not BOOTSTRAP_PS1.is_file():
        return [f"missing {BOOTSTRAP_PS1.relative_to(REPO).as_posix()}"]
    sh_text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    ps1_text = BOOTSTRAP_PS1.read_text(encoding="utf-8")
    sh_scannable = list(_iter_scannable_lines(sh_text))
    ps1_scannable = list(_iter_scannable_lines(ps1_text))
    problems = []
    for leaf, value in _iter_leaf_paths(manifest):
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
            continue
        for literal in sorted(_distinctive_literals(value)):
            for rel, scannable in (
                (BOOTSTRAP_SH.relative_to(REPO).as_posix(), sh_scannable),
                (BOOTSTRAP_PS1.relative_to(REPO).as_posix(), ps1_scannable),
            ):
                for lineno, (line, is_comment) in enumerate(scannable, start=1):
                    if is_comment or literal not in line:
                        continue
                    problems.append(
                        f"{rel}:{lineno}: hardcodes {literal!r}, a fragment of "
                        f"metadata/bootstrap.json leaf {leaf!r}, outside a comment -- this "
                        f"leaf is already read from the manifest by these scripts; "
                        f"a second, hardcoded copy of the same fact is exactly the drift "
                        f"issue #965 exists to catch -- derive it from the manifest instead"
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
    ap.add_argument(
        "--fix", action="store_true",
        help="Rewrite west.yml, the CI workflow --mr/cache-key pins, and the README badge "
             "FROM metadata/bootstrap.json's zephyr.version instead of only verifying them. "
             "Idempotent; run the gate without --fix afterwards to confirm.",
    )
    args = ap.parse_args()

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

    if args.fix:
        return _run_fix(manifest_version)

    problems = []
    problems += _check_west_yml(manifest_version)
    problems += _check_no_hardcoded_literal(BOOTSTRAP_SH, manifest_version)
    problems += _check_no_hardcoded_literal(BOOTSTRAP_PS1, manifest_version)
    problems += _check_readme_badge(manifest_version)
    problems += _check_prerequisites_posix(manifest)
    problems += _check_prerequisites_windows(manifest)
    problems += _check_python_min_version_posix(manifest)
    problems += _check_python_min_version_windows(manifest)
    problems += _check_install_commands(manifest)
    problems += _check_no_orphaned_leaves(manifest)
    problems += _check_native_lib_hints_consumption(manifest)
    for wf in CI_WORKFLOWS:
        problems += _check_ci_workflow(wf, manifest_version)
    problems += _check_library_versions(manifest_version)

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
