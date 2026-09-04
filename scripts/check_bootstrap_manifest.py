#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/bootstrap.json in lockstep with everything that copies it.

metadata/bootstrap.json (issue #917) is the single source of truth for the
Zephyr-workspace-assembly FACTS scripts/bootstrap.sh, scripts/bootstrap.ps1,
and Python Tan read today. Without a drift gate the manifest is
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
  6. `prerequisites.posix` disagrees with bootstrap.sh's hardcoded (canonical,
     non-Darwin) `REQUIRED_BINS=(...)`, `prerequisites.macos` disagrees with
     bootstrap.sh's Darwin-branch `REQUIRED_BINS=(...)` reassignment (the
     macOS xz/wget exemption), `prerequisites.windows` disagrees with
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
  11. `prerequisites.install` (issue #949, restructured for `install.linux`
      by issue #1464) -- the single source for every per-tool install
      COMMAND (as opposed to point 6 above, which only polices the tool
      NAME lists) -- disagrees with anything that copies one of those
      commands: a tool listed in `prerequisites.windows` or
      `prerequisites.posix` with no matching `install.windows.<tool>` /
      `install.linux.apt.<tool>` / `install.macos.<tool>` entry;
      `scripts/bootstrap.ps1`'s own hardcoded `$Prereqs` `Hint = "..."`
      value for a tool disagreeing with `install.windows.<tool>`; or one of
      `install.windows`'s own winget PACKAGE IDs (derived from the manifest,
      never hardcoded a second time) appearing in the scanned doc/script
      file set without its full canonical command alongside it -- or one
      of those PACKAGE IDs failing to extract from its own `install.windows`
      command in the first place. The literal scan covers the windows/
      winget side only; see `_check_install_commands`'s own docstring for
      why, the full assertion list, and the exact file set scanned.

      `install.linux` (issue #1464) is keyed by PACKAGE MANAGER, not by
      tool -- `install.linux.apt` / `install.linux.dnf` -- because a `sudo
      apt-get install`-shaped command is wrong on a host with no apt at all
      (Fedora/Arch/Rocky measured byte-identical to Debian before this
      fix). `install.linux.apt` is required and pinned to
      `prerequisites.posix` by EXACT key equality, the same bar
      `install.macos` already meets. `install.linux.dnf` is optional and,
      when present, its keys must be a SUBSET of `prerequisites.posix` --
      NOT required to be complete: a dnf-family tool this manifest cannot
      verify uniformly (today: `ninja` -- Fedora's own repos carry
      `ninja-build`, the RHEL-derivative default repos do not, without
      EPEL) is left out entirely rather than guessed. See
      `metadata/schemas/bootstrap-v1.schema.json`'s `install.linux`
      description for the full rationale, including why there is
      deliberately no `pacman` key and no distro-ID layer inside `dnf`.
  12. `scripts/bootstrap.sh`'s own hardcoded POSIX-side hint table (issue
      #978, restructured for the package-manager split by issue #1464) --
      `PREREQ_HINT_NAMES` / `PREREQ_HINT_APT` / `PREREQ_HINT_DNF` /
      `PREREQ_HINT_MACOS`, the bash analogue of bootstrap.ps1's `$Prereqs`
      `Hint=` field -- must agree entry-for-entry (matched up by array
      position, since bash 3.2 has no associative arrays) with
      `install.linux.apt` / `install.linux.dnf` / `install.macos`.
      `PREREQ_HINT_APT` must be COMPLETE (one entry per
      `prerequisites.posix` tool, mirroring `install.linux.apt`'s own
      completeness bar); `PREREQ_HINT_DNF` carries an empty string `""` at
      any position `install.linux.dnf` has no entry for -- asserted
      explicitly, not merely tolerated, so a future edit cannot quietly
      replace that intentional gap with a guessed command that this gate
      would otherwise wave through. See `_check_bootstrap_sh_install_hints`.
  13. `zephyr.pythonMinVersion` (issue #1078) -- the Zephyr-SCOPED Python
      floor, deliberately separate from the host-universal
      `prerequisites.pythonMinVersion` point 6 already polices (see that
      key's own schema description for why the two must not be unified) --
      disagrees with the REAL `PYTHON_MINIMUM_REQUIRED` the pinned Zephyr
      revision's own `cmake/modules/python.cmake` hardcodes, read with
      `git show <rev>:cmake/modules/python.cmake` from a Zephyr checkout
      resolved the same way `scripts/check_toolchain_lock.py`'s
      `_resolve_zephyr_dir` does (`$ZEPHYR_BASE`, falling back to the
      west-workspace topdir's conventional `zephyr/` project directory) --
      never the working tree's currently-checked-out ref, for the identical
      stale-local-clone reason that check's own `SDK_VERSION` cross-check
      already documents. Like that check, this is skip-not-fail when no such
      checkout resolves (a bare `pip install` or a pure-Python CI job
      legitimately has none) -- reusing the SAME `ALP_REQUIRE_ZEPHYR_ORACLE=1`
      escape hatch to turn the skip into a hard failure for a job that
      promises a Zephyr checkout, rather than inventing a second flag for
      the same concept. See `_check_zephyr_python_min_version`'s own
      docstring for the full policy. `zephyr.pythonMinVersion` is exempted
      from point 7's generic orphan-leaf scan (`_GATE_ASSERTED_LEAVES`) the
      same way `prerequisites.*` is -- neither bootstrap script reads or
      enforces it; this cross-check is its gate instead.
  14. `tools/native-sim-container/Containerfile`'s `ARG ZEPHYR_REV=...`
      default (issue #1458) disagrees with `zephyr.version`. This is the
      SAME category of machine pin point 4 already polices for the four CI
      workflows -- a hardcoded copy of the Zephyr revision, independent of
      west.yml -- just living in a fifth file point 4's curated CI_WORKFLOWS
      list was never meant to reach (a Containerfile is not a
      `.github/workflows/*.yml`). It shipped stuck one patch release behind
      (`v4.4.0` while west.yml/pr-twister.yml already carried `v4.4.1`) with
      nothing to catch it -- exactly the silent-drift shape this whole
      script exists to close. `tools/native-sim-container/Makefile`'s `build`
      target derives the value LIVE from west.yml and passes it as
      `--build-arg` on every real `make build`, so this default only matters
      for a standalone `docker build`/`podman build` that bypasses the
      Makefile -- but "only matters sometimes" is exactly what let it drift
      unnoticed before, so it stays gated like every other pin here. See
      `_check_containerfile`.
  15. `artifactProvenance` (issue #1574, ADR 0021 §3's one-consent-screen
      artifact/source/size/licence facts) has a key set that disagrees with
      the union of tool names `prerequisites.install.linux.apt` /
      `.linux.dnf` / `.macos` / `.windows` actually name an install command
      for -- a tool gaining an install command with no matching provenance
      entry, or a stale provenance entry for a tool no install command
      names anymore, fails here (`_check_artifact_provenance`), the same
      drift bar point 11 already holds `prerequisites.posix/macos/windows`
      to, one level further out. `artifactProvenance.*` is exempted from
      point 7's generic per-script orphan-leaf scan
      (`_PRODUCER_ONLY_LEAF_PREFIX`), for a DIFFERENT reason than
      `prerequisites.*`/`zephyr.pythonMinVersion` are: those two are read
      by neither script BY DESIGN (bootstrapping-before-the-manifest-is-
      trustworthy, and a derived-not-declared value, respectively) and have
      their own dedicated cross-check instead; `artifactProvenance` has no
      consumer in this repo AT ALL yet -- it is producer-only data for a
      future IDE/tan consumer (a companion tan-cli issue, filed only once
      this shape has proven itself) -- so its gate is "stays in lockstep
      with prerequisites.install", not "is read by bootstrap.sh/ps1".

--fix propagates a changed `zephyr.version` OUT to every machine pin site
this gate verifies above (points 2, 4, 5, 10, 14 -- west.yml, the CI
workflow `--mr`/cache-key pins, the README badge, every
in-tree-Zephyr-subsystem library manifest's `version:` field, and the
native-sim-container Containerfile's `ARG ZEPHYR_REV` default). It reuses
the exact same compiled regexes/constants the verify-only checks read
(`_WEST_YML_ZEPHYR_RE`, `_WEST_MR_RE`, `_CACHE_KEY_RE`, `_README_BADGE_RE`,
`_LIBRARY_VERSION_RE`, `_CONTAINERFILE_ZEPHYR_REV_RE`) -- there is
deliberately no second, parallel pin map; that would just be a new flavour
of the drift issue #917 exists to kill. Idempotent (a site already at
zephyr.version is left untouched, byte-for-byte); a site the gate expects
but can no longer find/match is a hard failure naming it, never a silent
no-op. bootstrap.sh and bootstrap.ps1 are NOT --fix sites -- they read
zephyr.version from the manifest at run time and must never hardcode it
(that's what point 3 above polices); prose docs, CHANGELOG history, and each
library manifest's own `# Grounding (pinned Zephyr ...)` provenance comment
are out of scope for a mechanical regex rewrite by design (see
docs/zephyr-version-policy.md) -- only the `version:` field itself is a fix
site.

`zephyr.pythonMinVersion` (point 13) is deliberately NOT a --fix site, and
the asymmetry is the point: every site above is one --fix WRITES, taking
metadata/bootstrap.json as the source of truth and propagating OUT to
machine pins. pythonMinVersion flows the other way -- it is derived FROM the
pinned Zephyr's own cmake/modules/python.cmake, so "fixing" it would mean
--fix reaching INTO the manifest it is elsewhere reading as authoritative.
Making one field flow inbound while every other flows outbound is exactly
the kind of two-directional pin map the paragraph above rules out. A Zephyr
bump is therefore: edit zephyr.version, run --fix (propagates outward), then
run the plain gate -- whose failure names the required value verbatim
("... disagrees with the pinned Zephyr vX.Y.Z's own PYTHON_MINIMUM_REQUIRED
'A.B'"), so the one manual edit is a copy of a value the gate just printed,
not a lookup anyone has to perform.

Run locally:

    python3 scripts/check_bootstrap_manifest.py
    python3 scripts/check_bootstrap_manifest.py --fix
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
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
# The native_sim reproduction container's own Zephyr-revision pin (issue
# #1458) -- see module docstring point 14.
CONTAINERFILE = REPO / "tools" / "native-sim-container" / "Containerfile"

# Every CI workflow that assembles its own throwaway Zephyr workspace and
# therefore pins a Zephyr revision independent of west.yml/west update.
CI_WORKFLOWS = [
    REPO / ".github" / "workflows" / "pr-twister.yml",
    REPO / ".github" / "workflows" / "pr-tier-a-libraries.yml",
    REPO / ".github" / "workflows" / "pr-getting-started-aen801.yml",
]
# `nightly-aen-hil.yml` was listed here until it was deleted (#968): CI no
# longer drives the AEN bench, because the bench is labgrid-reservation-gated
# and strictly serial and SETOOLS is licence-gated and not redistributable.
# This list is a hardcoded set of paths, so a deleted workflow does not drop
# out of it -- it becomes `missing .github/workflows/<name>` and hard-fails.
# `pr-twister.yml` runs this gate and is a REQUIRED context, so a stale entry
# here reds every PR.

# Every top-level manifest key MUST be listed here -- main()'s unknown-key
# check below fails loudly if a key is added without anyone deciding how it
# gets policed (consumed by both scripts, or asserted equal to a hardcoded
# copy). This is the generalisation past just zephyr.version (issue #917).
KNOWN_KEYS = {
    "_comment", "schemaVersion", "zephyr", "venv", "prerequisites",
    "artifactProvenance", "west", "pip", "verdict", "env", "nativeLibHints",
    "manualInstallHints",
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
# tools/native-sim-container/Containerfile's `ARG ZEPHYR_REV=...` default
# (module docstring point 14, issue #1458).
_CONTAINERFILE_ZEPHYR_REV_RE = re.compile(r"ARG\s+ZEPHYR_REV=(\S+)")

# Leaves that stay hardcoded in both scripts BY DESIGN (see point 6 in the
# module docstring) -- policed by their own dedicated comparison checks
# below instead of the generic orphan-leaf scan.
_GATE_ASSERTED_LEAF_PREFIX = "prerequisites."
# Individual leaves (as opposed to a whole `prerequisites.*` subtree) that
# are ALSO gate-asserted-instead rather than read by either script -- see
# point 13 in the module docstring. `zephyr.pythonMinVersion` is checked
# against the pinned Zephyr's own PYTHON_MINIMUM_REQUIRED by
# `_check_zephyr_python_min_version`, not read at run time by bootstrap.sh
# or bootstrap.ps1 (neither enforces a Zephyr-scoped floor today; that is
# tan-cli's still-outstanding half of issue #1078).
_GATE_ASSERTED_LEAVES = {"zephyr.pythonMinVersion"}
# `artifactProvenance.*` (issue #1574, module docstring point 15): producer-
# only data with NO consumer in this repo yet -- neither bootstrap.sh nor
# bootstrap.ps1 has any reason to read it, since it is ADR 0021 §3's
# IDE/tan-facing consent-screen data, not workspace-assembly control flow.
# Gate-asserted-instead by `_check_artifact_provenance` (keyed against
# `prerequisites.install`'s own tool-name set) rather than by the generic
# per-script-read scan, which would otherwise demand a reader that has no
# reason to exist yet.
_PRODUCER_ONLY_LEAF_PREFIX = "artifactProvenance."
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


def _check_containerfile(manifest_version: str) -> list[str]:
    """Point 14 (module docstring, issue #1458):
    tools/native-sim-container/Containerfile's `ARG ZEPHYR_REV=...` default
    must agree with `zephyr.version`. `tools/native-sim-container/Makefile`'s
    `build` target derives the real value LIVE from west.yml on every `make
    build`, so this default only ever fires for a standalone `docker
    build`/`podman build` that bypasses the Makefile -- but that is exactly
    the path issue #1458 found silently stuck a patch release stale, so it
    is gated the same as every other machine pin this script polices."""
    if not CONTAINERFILE.is_file():
        return [f"missing {CONTAINERFILE.relative_to(REPO).as_posix()}"]
    rel = CONTAINERFILE.relative_to(REPO).as_posix()
    text = CONTAINERFILE.read_text(encoding="utf-8")
    m = _CONTAINERFILE_ZEPHYR_REV_RE.search(text)
    if m is None:
        return [f"{rel}: no `ARG ZEPHYR_REV=...` default found -- update this "
                 f"gate if the pin format changed"]
    containerfile_version = m.group(1)
    if containerfile_version != manifest_version:
        return [f"{rel} pins ARG ZEPHYR_REV {containerfile_version!r}, "
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

    r, p = _apply_version_fix(CONTAINERFILE, _CONTAINERFILE_ZEPHYR_REV_RE, manifest_version,
                               site_desc="the `ARG ZEPHYR_REV=...` default")
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


# bootstrap.sh's Darwin branch reassigns REQUIRED_BINS to a narrower set (no
# xz/wget) -- `_SH_REQUIRED_BINS_RE` above is a plain `.search` (first match
# wins, BY DESIGN: it targets the canonical, non-Darwin literal), so it can
# never see this second assignment. Anchored on the `"Darwin" ]; then` guard
# so it can't accidentally match the canonical or --print-env arrays instead.
_SH_DARWIN_REQUIRED_BINS_RE = re.compile(
    r'"Darwin"\s*\]\s*;\s*then.*?REQUIRED_BINS=\(([^)]*)\)', re.DOTALL)


def _check_prerequisites_macos(manifest: dict) -> list[str]:
    """scripts/bootstrap.sh's Darwin-branch `REQUIRED_BINS=(...)`
    reassignment (the macOS xz/wget exemption) must agree with
    `prerequisites.macos` -- the single source of truth for the EFFECTIVE
    macOS prerequisite set, the same way `prerequisites.posix` polices the
    canonical (non-Darwin) `REQUIRED_BINS` literal `_check_prerequisites_posix`
    above already checks. Without this, `prerequisites.macos` could drift
    (a tool added or dropped on either side) with nothing to catch it --
    exactly the hole `prerequisites.posix`/`.windows` already close for the
    non-Darwin lists."""
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    m = _SH_DARWIN_REQUIRED_BINS_RE.search(text)
    if m is None:
        return ["scripts/bootstrap.sh: could not find the Darwin-branch "
                 "`REQUIRED_BINS=(...)` reassignment -- update this gate if "
                 "it was renamed/restructured"]
    sh_bins = set(m.group(1).split())
    manifest_bins = set(manifest["prerequisites"]["macos"])
    if sh_bins != manifest_bins:
        return [f"scripts/bootstrap.sh Darwin-branch REQUIRED_BINS={sorted(sh_bins)} "
                 f"disagrees with metadata/bootstrap.json prerequisites.macos="
                 f"{sorted(manifest_bins)}"]
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


# -------- zephyr.pythonMinVersion (issue #1078) -------------------------------
#
# Unlike prerequisites.pythonMinVersion above (checked against a hardcoded
# copy inside each bootstrap script), zephyr.pythonMinVersion has no such
# copy anywhere in THIS repo to check against -- its ground truth lives in
# the pinned Zephyr checkout itself. Derived and verified the same way
# scripts/check_toolchain_lock.py's own zephyrSdk.version <-> SDK_VERSION
# cross-check is: `git show <pinned rev>:<path>` from a resolved Zephyr
# checkout, never the working tree's currently-checked-out ref (a local dev
# clone routinely sits on a different tag than the pin).

_PYTHON_CMAKE_PATH = "cmake/modules/python.cmake"
_ZEPHYR_PYTHON_MIN_RE = re.compile(r"set\(\s*PYTHON_MINIMUM_REQUIRED\s+(\d+\.\d+)\s*\)")


def _resolve_zephyr_dir() -> Path:
    """Same resolution order as `scripts/check_toolchain_lock.py`'s
    `_resolve_zephyr_dir` (and `tests/scripts/test_hil_blocks_coverage.py`'s
    `_pinned_zephyr_sysbuild_kconfig_symbols`): `$ZEPHYR_BASE` (the
    convention every `west` command and `tan doctor` use),
    falling back to the west-workspace topdir's conventional `zephyr/`
    project directory (`scripts/bootstrap.sh` does `west init -l <alp-sdk>`,
    so alp-sdk's parent is the topdir). Duplicated here rather than
    imported from that sibling gate script -- each check_*.py gate stays a
    standalone entry point, not cross-coupled to another gate's module."""
    env_base = os.environ.get("ZEPHYR_BASE")
    return Path(env_base) if env_base else REPO.parent / "zephyr"


def _zephyr_python_min_in_pinned_zephyr(zephyr_dir: Path, pinned_version: str) -> str | None:
    """Read `PYTHON_MINIMUM_REQUIRED` at `pinned_version` straight from
    `zephyr_dir`'s git object store via
    `git show <rev>:cmake/modules/python.cmake`, never from the working
    tree's currently-checked-out files -- same technique and same rationale
    as check_toolchain_lock.py's `_sdk_version_in_pinned_zephyr`. Returns
    None (unresolvable, never a hard error) when `zephyr_dir` doesn't exist,
    isn't a git checkout, doesn't have `pinned_version` as a reachable ref,
    or the file at that revision has no `set(PYTHON_MINIMUM_REQUIRED X.Y)`
    line to match -- any of these means "no oracle available", the same
    single outcome `_check_zephyr_python_min_version` treats as skip-or-fail
    depending on ALP_REQUIRE_ZEPHYR_ORACLE."""
    if not (zephyr_dir / ".git").exists():
        return None
    try:
        result = subprocess.run(
            ["git", "-C", str(zephyr_dir), "show", f"{pinned_version}:{_PYTHON_CMAKE_PATH}"],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    m = _ZEPHYR_PYTHON_MIN_RE.search(result.stdout)
    return m.group(1) if m else None


def _check_zephyr_python_min_version(manifest: dict) -> tuple[list[str], str | None]:
    """Point 13 (module docstring). Returns (problems, skip_reason).

    Skip-vs-fail policy, reused verbatim from check_toolchain_lock.py's
    `_check_sdk_version_matches_zephyr_pin` (same escape hatch, same
    reasoning -- see that function's own docstring for the full
    justification): whether a pinned Zephyr checkout is resolvable at all
    is an ENVIRONMENT FACT, not a manifest defect, so the default is
    skip-with-reason. A job that DOES assemble a Zephyr workspace before
    running this gate can set `ALP_REQUIRE_ZEPHYR_ORACLE=1` to turn that
    skip into a hard failure -- there, an unresolvable checkout is a bug in
    the job's own setup, not a fact of the environment.

    A skip is always printed by `main()` (never silently swallowed) so it
    cannot quietly become permanent -- exactly what issue #1078 asked for."""
    pinned_zephyr_version = manifest["zephyr"]["version"]
    zephyr_dir = _resolve_zephyr_dir()
    pinned_floor = _zephyr_python_min_in_pinned_zephyr(zephyr_dir, pinned_zephyr_version)

    if pinned_floor is None:
        # Build the two messages independently. Slicing the skip reason to
        # reuse it as the failure reason inverts it -- dropping the leading
        # "no " asserts the checkout WAS resolved, and leaves the sentence
        # claiming it is "skipping" while the function is hard-failing.
        missing = (
            f"no Zephyr checkout resolved at {zephyr_dir} with "
            f"{pinned_zephyr_version} as a reachable git revision carrying a "
            f"`set(PYTHON_MINIMUM_REQUIRED X.Y)` line in {_PYTHON_CMAKE_PATH}"
        )
        if os.environ.get("ALP_REQUIRE_ZEPHYR_ORACLE") == "1":
            return (
                [
                    f"ALP_REQUIRE_ZEPHYR_ORACLE=1 but {missing} -- this job "
                    f"promised the oracle and did not deliver it; fix the "
                    f"job's checkout, do not drop the flag"
                ],
                None,
            )
        return [], (
            f"{missing} -- skipping the metadata/bootstrap.json "
            f"zephyr.pythonMinVersion <-> Zephyr's own "
            f"PYTHON_MINIMUM_REQUIRED cross-check"
        )

    manifest_floor = manifest["zephyr"]["pythonMinVersion"]
    if pinned_floor != manifest_floor:
        return (
            [
                f"metadata/bootstrap.json zephyr.pythonMinVersion {manifest_floor!r} "
                f"disagrees with the pinned Zephyr {pinned_zephyr_version}'s own "
                f"PYTHON_MINIMUM_REQUIRED {pinned_floor!r} (git -C {zephyr_dir} show "
                f"{pinned_zephyr_version}:{_PYTHON_CMAKE_PATH})"
            ],
            None,
        )
    return [], None


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

# The exhaustive allowlist of `prerequisites.install.windows` keys with no
# matching `prerequisites.windows` gate entry (issue #1036's superset
# relaxation). `_check_install_commands` point 1 checks completeness in the
# gate -> install direction only (every gated tool needs an install command);
# without a bound on the REVERSE direction, ANY key here -- typo'd, garbage,
# or a stale leftover from a tool that stopped gating bootstrap -- would sit
# undetected forever, since nothing else in this repo notices an
# `install.windows` entry with no reader (the schema's
# `additionalProperties: {type: string, minLength: 1}` accepts any key name,
# and the $Prereqs / literal-scan checks below only ever walk FROM
# prerequisites.windows / install.windows's current values, never flag an
# extra key that isn't on either list).
#
#   "7zip": gates `west sdk install` (patoolib's external 7z/7za/7zr/7zz/
#   7zzs/unar shell-out for `.7z` extraction, no pure-Python fallback), a
#   separate manual step `bootstrap.ps1` deliberately never runs -- so 7-Zip
#   must NOT be added to `prerequisites.windows` (that would make
#   bootstrap.ps1 refuse on every Windows host lacking 7-Zip, for a tool
#   bootstrap itself never touches). See metadata/bootstrap.json's
#   `prerequisites.install.windows.7zip` and
#   metadata/schemas/bootstrap-v1.schema.json's `install.windows`
#   description.
#
# Add a NEW entry here only if the tool gates something bootstrap.ps1
# deliberately doesn't run (the 7zip shape) -- if it should instead refuse
# bootstrap without it, add it to `prerequisites.windows` (and a matching
# $Prereqs entry in scripts/bootstrap.ps1), not here.
_WINDOWS_INSTALL_ONLY_TOOLS = {"7zip"}


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

      1. Completeness -- every `prerequisites.windows` tool has a matching
         `install.windows` entry, `install.macos`'s keys equal
         `prerequisites.posix`'s tools, and `install.linux.apt`'s keys ALSO
         equal `prerequisites.posix`'s tools (issue #1464 -- `install.linux`
         is keyed by package manager, not tool; `apt` carries the same
         completeness bar `install.macos` always has, since it is the
         Debian/Ubuntu-family default every existing consumer already
         depends on). `install.linux.dnf`, when present, is checked in the
         OTHER direction only -- every key it DOES carry must be one of
         `prerequisites.posix`'s tools (a typo'd/stale/unknown tool name is
         still rejected) -- but it is NOT required to be complete: a
         dnf-family tool this manifest cannot verify with one
         Fedora-and-RHEL-derivative-uniform command (today: `ninja`) is
         correctly represented by its ABSENCE from the map, not by a
         guessed entry. `install.linux.dnf` itself is optional -- the
         schema does not `require` it -- consistent with "prove it or leave
         it out" at the sub-map level too. The gate -> install direction is
         subset-only for windows: `install.windows` MAY carry additional
         entries with no matching `prerequisites.windows` gate --
         issue #1036's `7zip` is the first live case, gating `west sdk
         install` rather than bootstrap itself, the same "install has a
         command but nothing refuses without it" shape `install.macos`'s
         `xz`/`wget` already established on the posix side (see
         metadata/schemas/bootstrap-v1.schema.json's `prerequisites.install`
         description for the full rationale). The REVERSE direction is
         bounded, not unchecked: any `install.windows` key that is neither
         in `prerequisites.windows` nor in this script's own
         `_WINDOWS_INSTALL_ONLY_TOOLS` allowlist is rejected -- without that
         bound a typo'd or stale `install.windows` entry (a garbage key, or
         a tool removed from `prerequisites.windows` and
         `scripts/bootstrap.ps1`'s `$Prereqs` but left behind in
         `install.windows`) would sit undetected forever. A tool with no
         install command at all is the exact hole that produced the drifted/
         incomplete ninja hint in the now-retired scripts/alp_cli/doctor.py this change
         fixes. This is the ONLY assertion covering install.linux /
         install.macos -- see point 3's own note on why.
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
         exactly the shape the now-retired scripts/alp_cli/doctor.py's old ninja hint
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
    missing_windows = windows_tools - windows_install
    if missing_windows:
        problems.append(
            f"prerequisites.install.windows is missing install command(s) for "
            f"{sorted(missing_windows)} -- every windows prerequisite needs its "
            f"own install command"
        )
    extra_windows = windows_install - windows_tools - _WINDOWS_INSTALL_ONLY_TOOLS
    if extra_windows:
        problems.append(
            f"prerequisites.install.windows has entr(y/ies) {sorted(extra_windows)} "
            f"with no matching prerequisites.windows gate and no "
            f"_WINDOWS_INSTALL_ONLY_TOOLS allowlist entry in this script -- either "
            f"add the tool to prerequisites.windows (if bootstrap.ps1 should refuse "
            f"without it) or to _WINDOWS_INSTALL_ONLY_TOOLS (if it gates something "
            f"bootstrap.ps1 deliberately never runs, the 7zip/`west sdk install` "
            f"shape)"
        )
    posix_tools = set(prereqs.get("posix", []))
    os_install = set(install.get("macos", {}))
    if os_install != posix_tools:
        problems.append(
            f"prerequisites.install.macos keys {sorted(os_install)} disagree "
            f"with prerequisites.posix tools {sorted(posix_tools)} -- every "
            f"posix prerequisite needs its own install command on macos"
        )

    # install.linux (issue #1464): keyed by PACKAGE MANAGER, not tool.
    # `apt` is required and held to the SAME exact-equality bar as
    # install.macos above (it is the Debian/Ubuntu-family default every
    # existing consumer -- bootstrap.sh, doctor.py, tan's byte-pinned
    # fallback -- already depends on). `dnf` is optional and, when present,
    # is checked in the OTHER direction only: every key it declares must be
    # a real prerequisites.posix tool, but it need not cover all of them --
    # a tool this manifest cannot verify with one command that works across
    # the whole dnf ecosystem (Fedora + RHEL-derivatives) is correctly
    # represented by leaving it OUT, not by guessing (see
    # metadata/schemas/bootstrap-v1.schema.json's install.linux description
    # for the ninja/EPEL evidence). Schema `additionalProperties: false`
    # already rejects any package-manager key other than apt/dnf (e.g. a
    # hand-added `pacman`) before this function ever runs -- see main()'s
    # early schema-validation return -- so this function does not repeat
    # that check.
    linux_install = install.get("linux", {})
    apt_install = set(linux_install.get("apt", {})) if isinstance(linux_install, dict) else set()
    if apt_install != posix_tools:
        problems.append(
            f"prerequisites.install.linux.apt keys {sorted(apt_install)} disagree "
            f"with prerequisites.posix tools {sorted(posix_tools)} -- every posix "
            f"prerequisite needs its own install command on linux.apt (the "
            f"Debian/Ubuntu-family default)"
        )
    if isinstance(linux_install, dict) and "dnf" in linux_install:
        dnf_install = set(linux_install["dnf"])
        unknown_dnf = dnf_install - posix_tools
        if unknown_dnf:
            problems.append(
                f"prerequisites.install.linux.dnf has entr(y/ies) {sorted(unknown_dnf)} "
                f"with no matching prerequisites.posix tool"
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
            # down: a GATED tool present in install.windows with no entry it
            # can be checked against must be named, not silently skipped by
            # the loop below.
            #
            # Deliberately walks `windows_tools` (the `prerequisites.windows`
            # gate list), not `windows_install_map`'s full key set: bootstrap.ps1
            # only ever prompts for a GATED tool (that's the entire reason
            # $Prereqs exists -- to refuse before python/JSON is even
            # confirmed present), so a superset `install.windows` entry with
            # no gate (issue #1036's `7zip`, which gates `west sdk install`,
            # not bootstrap.ps1) has no reason to appear in $Prereqs at all;
            # requiring one here would force a bootstrap-time prompt for a
            # tool bootstrap.ps1 never needs.
            parsed_names = {name for name, _ in entries}
            for tool in sorted(windows_tools):
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


# -------- scripts/bootstrap.sh PREREQ_HINT_* agreement (issue #978) ----------
#
# The POSIX-side analogue of `_check_install_commands`'s point 2
# (bootstrap.ps1's own `$Prereqs` `Hint=` agreement). bootstrap.sh's hint
# table is FOUR PARALLEL arrays (was three -- issue #1464 split the old
# single Linux array by PACKAGE MANAGER), not one `Name=...; Hint=...` pair
# per line the way `$Prereqs` is (bash 3.2 -- the macOS-shipped version --
# has no `declare -A`), so this is matched up by array POSITION rather than
# a per-entry regex.
_SH_PREREQ_HINT_NAMES_RE = re.compile(r"PREREQ_HINT_NAMES=\(([^)]*)\)")
_SH_PREREQ_HINT_APT_RE = re.compile(r"PREREQ_HINT_APT=\((.*?)\n\)", re.DOTALL)
_SH_PREREQ_HINT_DNF_RE = re.compile(r"PREREQ_HINT_DNF=\((.*?)\n\)", re.DOTALL)
_SH_PREREQ_HINT_MACOS_RE = re.compile(r"PREREQ_HINT_MACOS=\((.*?)\n\)", re.DOTALL)
# `[^"]*` (zero-or-more) rather than `[^"]+` -- matches an EMPTY `""` slot
# too (issue #1464 -- PREREQ_HINT_DNF carries one for a tool install.linux.
# dnf declares no command for, e.g. `ninja`), capturing `""` as the empty
# string rather than skipping that array entry entirely.
_SH_QUOTED_STRING_RE = re.compile(r'"([^"]*)"')


def _check_bootstrap_sh_install_hints(manifest: dict) -> list[str]:
    """`scripts/bootstrap.sh`'s `PREREQ_HINT_NAMES` / `PREREQ_HINT_APT` /
    `PREREQ_HINT_DNF` / `PREREQ_HINT_MACOS` (issue #978, package-manager
    split by issue #1464) must agree, entry-for-entry, with
    `prerequisites.install.linux.apt` / `.linux.dnf` / `.macos`. Each array
    is parsed independently and then zipped by index -- a length mismatch
    between `PREREQ_HINT_NAMES` and any hint array is reported directly
    rather than silently truncated by `zip`.

    Also asserts completeness (mirrors `_check_install_commands` point 2's
    own `parsed_names` loop, same "goes dark" failure one level down): every
    tool key `prerequisites.install.linux.apt` / `.linux.dnf` / `.macos`
    DOES carry must have a `PREREQ_HINT_NAMES` entry, not just every
    `PREREQ_HINT_NAMES` entry a matching install key (which the zip loop
    below already covers). Without this, a tool added to one of those maps +
    `REQUIRED_BINS` but left out of the hint table falls through
    bootstrap.sh's bare-name `warn "  ${bin}"` branch with this gate
    reporting rc=0 -- the #978 defect, restored.

    `PREREQ_HINT_DNF` is allowed one shape none of the other three arrays
    are: an entry whose manifest counterpart is genuinely ABSENT (no
    `install.linux.dnf.<tool>` key at all -- `dnf` is an optional, partial
    map, issue #1464) must pair with an EMPTY STRING `""` at that position,
    never a real command -- a real command there with nothing in the
    manifest backing it would be exactly the pre-#949 drift (a hardcoded
    hint with no single source of truth) reintroduced one ecosystem later.
    `install.linux.apt` / `.macos` are both REQUIRED-complete maps (checked
    by `_check_install_commands` point 1), so this same "absent means
    empty" allowance applies to them structurally too but should never
    actually fire in a well-formed manifest."""
    if not BOOTSTRAP_SH.is_file():
        return [f"missing {BOOTSTRAP_SH.relative_to(REPO).as_posix()}"]
    text = BOOTSTRAP_SH.read_text(encoding="utf-8")
    problems: list[str] = []

    m_names = _SH_PREREQ_HINT_NAMES_RE.search(text)
    if m_names is None:
        return ["scripts/bootstrap.sh: could not find `PREREQ_HINT_NAMES=(...)` "
                 "-- update this gate if it was renamed/restructured"]
    names = m_names.group(1).split()
    parsed_names = set(names)

    install = manifest.get("prerequisites", {}).get("install", {})
    linux_install = install.get("linux", {})
    if not isinstance(linux_install, dict):
        linux_install = {}
    for pm_key, pm_re, os_install in (
        ("apt", _SH_PREREQ_HINT_APT_RE, linux_install.get("apt", {})),
        ("dnf", _SH_PREREQ_HINT_DNF_RE, linux_install.get("dnf", {})),
        ("macos", _SH_PREREQ_HINT_MACOS_RE, install.get("macos", {})),
    ):
        # install.linux.<pm>.<tool> in the problem text below (not
        # install.<pm>.<tool>) for apt/dnf, so a reported path is always the
        # real manifest path a reader can grep for; macos keeps its own flat
        # install.macos.<tool> path.
        manifest_prefix = f"linux.{pm_key}" if pm_key in ("apt", "dnf") else pm_key
        if not isinstance(os_install, dict):
            os_install = {}
        m = pm_re.search(text)
        if m is None:
            problems.append(
                f"scripts/bootstrap.sh: could not find `PREREQ_HINT_{pm_key.upper()}=(...)` "
                f"-- update this gate if it was renamed/restructured"
            )
            continue
        hints = _SH_QUOTED_STRING_RE.findall(m.group(1))
        for tool in sorted(os_install):
            if tool not in parsed_names:
                problems.append(
                    f"prerequisites.install.{manifest_prefix}.{tool} has no "
                    f"PREREQ_HINT_NAMES entry in scripts/bootstrap.sh -- it "
                    f"would fall through to the bare-name warn() branch "
                    f"(issue #978) instead of printing an install hint"
                )
        if len(hints) != len(names):
            problems.append(
                f"scripts/bootstrap.sh PREREQ_HINT_{pm_key.upper()} has {len(hints)} "
                f"entries but PREREQ_HINT_NAMES has {len(names)} -- they must stay "
                f"parallel arrays"
            )
            continue
        for name, hint in zip(names, hints):
            canonical = os_install.get(name)
            if canonical is None:
                # Intentional-gap allowance is DNF-ONLY (review finding on
                # #1471): install.linux.dnf may be a partial map, and an
                # unbacked "" slot there is exactly the correct rendering of
                # "this manifest ships no verified command for this tool on
                # this package manager" -- not a problem to report. apt and
                # macos are both REQUIRED-complete maps (point 1 of
                # `_check_install_commands` pins both to `prerequisites.
                # posix` by exact key equality), so a canonical-less entry on
                # either of THOSE sides is never legitimate -- gating this
                # allowance to dnf keeps them exactly as strict as before
                # this map ever had a partial-allowed sibling. Un-gated, a
                # stray PREREQ_HINT_APT/_MACOS entry blanked to `""` for a
                # tool the manifest doesn't declare would silently pass here
                # instead of being reported.
                if hint == "" and pm_key == "dnf":
                    continue
                problems.append(
                    f"scripts/bootstrap.sh PREREQ_HINT_{pm_key.upper()} has an entry "
                    f"for {name!r}, but metadata/bootstrap.json has no "
                    f"prerequisites.install.{manifest_prefix}.{name}"
                )
            elif hint != canonical:
                problems.append(
                    f"scripts/bootstrap.sh PREREQ_HINT_{pm_key.upper()} entry {name!r} "
                    f"= {hint!r} disagrees with prerequisites.install.{manifest_prefix}.{name} "
                    f"= {canonical!r}"
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
        if (leaf in _STRUCTURAL_LEAVES or leaf.startswith(_GATE_ASSERTED_LEAF_PREFIX)
                or leaf.startswith(_PRODUCER_ONLY_LEAF_PREFIX)
                or leaf in _GATE_ASSERTED_LEAVES):
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


def _check_artifact_provenance(manifest: dict) -> list[str]:
    """`artifactProvenance` (issue #1574, module docstring point 15) must
    name exactly the tools `prerequisites.install` actually ships an
    install command for -- the union of `install.linux.apt` /
    `install.linux.dnf` / `install.macos` / `install.windows` keys, not
    `prerequisites.posix/macos/windows` directly (those three lists mix
    `python3` and `python` for the same upstream project, the identical
    split `install.*` and therefore `artifactProvenance` already make).
    Catches a tool gaining an install command with no provenance entry
    (silently unable to render ADR 0021 §3's consent screen for it) and a
    stale provenance entry for a tool no install command names anymore, the
    same two-directional drift bar `_check_install_commands` already holds
    `prerequisites.posix/macos/windows` to for the install commands
    themselves."""
    install = manifest["prerequisites"]["install"]
    expected: set[str] = set()
    expected |= set(install["linux"]["apt"].keys())
    expected |= set(install["linux"].get("dnf", {}).keys())
    expected |= set(install["macos"].keys())
    expected |= set(install["windows"].keys())
    actual = set(manifest.get("artifactProvenance", {}).keys())
    problems = []
    missing = expected - actual
    if missing:
        problems.append(
            f"artifactProvenance is missing entries for {sorted(missing)} -- every tool "
            f"prerequisites.install names an install command for needs a "
            f"tier/source/sizeBytes/licence entry (ADR 0021 §3, issue #1574)"
        )
    extra = actual - expected
    if extra:
        problems.append(
            f"artifactProvenance has stale entries for {sorted(extra)} -- no "
            f"prerequisites.install.* map names an install command for them anymore; "
            f"remove the entry or restore the matching install command"
        )
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
        help="Rewrite west.yml, the CI workflow --mr/cache-key pins, the README badge, and "
             "the native-sim-container Containerfile's ARG ZEPHYR_REV default FROM "
             "metadata/bootstrap.json's zephyr.version instead of only verifying them. "
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
    problems += _check_prerequisites_macos(manifest)
    problems += _check_prerequisites_windows(manifest)
    problems += _check_python_min_version_posix(manifest)
    problems += _check_python_min_version_windows(manifest)
    problems += _check_install_commands(manifest)
    problems += _check_bootstrap_sh_install_hints(manifest)
    problems += _check_artifact_provenance(manifest)
    problems += _check_no_orphaned_leaves(manifest)
    problems += _check_native_lib_hints_consumption(manifest)
    for wf in CI_WORKFLOWS:
        problems += _check_ci_workflow(wf, manifest_version)
    problems += _check_library_versions(manifest_version)
    problems += _check_containerfile(manifest_version)

    zephyr_python_problems, zephyr_python_skip = _check_zephyr_python_min_version(manifest)
    problems += zephyr_python_problems

    if problems:
        print(f"FAIL bootstrap manifest drift (metadata/bootstrap.json declares "
              f"zephyr.version {manifest_version!r}):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1

    if zephyr_python_skip is not None:
        print(f"check_bootstrap_manifest: SKIP -- {zephyr_python_skip}")

    print(f"check_bootstrap_manifest: OK -- metadata/bootstrap.json, west.yml, README.md, "
          f"scripts/bootstrap.sh, scripts/bootstrap.ps1, "
          f"tools/native-sim-container/Containerfile, and {len(CI_WORKFLOWS)} "
          f"CI workflow(s) all agree on Zephyr {manifest_version}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
