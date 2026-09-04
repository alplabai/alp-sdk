#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
One-command release-prep tool.

Bumps the SDK version in every place that tracks it, regenerates the
ABI snapshot for the new version, and (optionally) creates the git
tag.  Doesn't push -- the operator does that explicitly so a bad bump
can be undone locally.

Workflow:

    # 0. Fold changelog.d/ fragments into CHANGELOG.md first (#1395) --
    #    slice_changelog() below refuses to run while any remain, but
    #    folding explicitly here keeps the diff reviewable before the bump.
    python3 scripts/assemble_changelog.py

    # 1. Verify everything looks ready (no-op dry run)
    python3 scripts/bump_version.py --to 1.0.0 --dry-run

    # 2. Apply the bump
    python3 scripts/bump_version.py --to 1.0.0

    # 3. Inspect, then commit + tag
    git diff --stat
    git add -A
    git commit -m "chore: bump version to 1.0.0"
    git tag -s v1.0.0 -m "v1.0.0"

    # (Optional, after pushing:)
    git push origin main --tags

Bump rules (per docs/release-policy.md):

    MAJOR   -> ABI-breaking change.
    MINOR   -> Additive ABI change (new symbols / new schema blocks).
    PATCH   -> Bug fix, no public-surface change.

The script doesn't enforce these -- the operator chooses the version
and the ABI workflow gates whether it was correct.

Pre-release cuts (issue #1902 -- a build made from an rc must identify
itself as one, not as its eventual GA version):

    python3 scripts/bump_version.py --to 1.0.0-rc1

Ordinary SemVer 2.0.0 pre-release syntax, not a bespoke flag.  This writes
the FULL "1.0.0-rc1" into metadata/sdk_version.yaml's `version:` and
include/alp/version.h's ALP_VERSION_STRING (ALP_VERSION_MAJOR/MINOR/PATCH
stay the plain core integers, 1/0/0).  pyproject.toml's `[project]` version
and the alp_banner.c sample line stay pinned to the core "1.0.0" -- PEP 440
doesn't accept a bare hyphenated suffix, and neither is the identification
surface; the banner's *code* always prints the live ALP_VERSION_STRING at
runtime regardless of what this static sample line says.  CHANGELOG.md is
left untouched: an rc has no GA section of its own yet, and slicing one
here would starve `.github/workflows/release.yml`'s documented pre-release
fallback (it reads `## [Unreleased]` when the core `## [vX.Y.Z]` heading
doesn't exist) of any content to publish.  The later GA bump
(`--to 1.0.0`, no suffix) does the real CHANGELOG slice.

What this touches:

    metadata/sdk_version.yaml       -- the declared version.
    CHANGELOG.md                    -- slice [Unreleased] into the new version section.
    docs/abi/v<MAJOR.MINOR>-snapshot.json  -- regenerated.
    include/alp/version.h           -- ALP_VERSION_MAJOR/MINOR/PATCH +
                                       ALP_VERSION_STRING macros;
                                       enforced by scripts/check_version_doc_sync.py.
    pyproject.toml                  -- the alp-sdk-cli [project] version;
                                       enforced by scripts/check_version_doc_sync.py.
    src/zephyr/alp_banner.c         -- the sample banner line in the
                                       file's doc-comment (the code always
                                       prints the live ALP_VERSION_STRING);
                                       enforced by scripts/check_version_doc_sync.py.
    tests/fixtures/emit-snapshots/  -- the `--emit` goldens (#1461):
                                       build-plan's sdkVersion field reads
                                       metadata/sdk_version.yaml directly, and
                                       a released scaffold's README doc links
                                       pin to v<version>, so both go stale on
                                       every bump; regenerated the same way
                                       `check_emit_snapshots.py --update`
                                       already does by hand, enforced by
                                       scripts/check_emit_snapshots.py.

The README/docs current-state prose is de-versioned (single-source
version derived from metadata/sdk_version.yaml), so bump touches no
version labels there; scripts/alp_cli/__init__.py derives __version__
from sdk_version.yaml at import time, so it needs no rewrite either.

What it does NOT touch:

    - Git history.  Use `git tag` separately.
    - VERSIONS.md.  That's the roadmap; rewriting it on every bump
      muddies the roadmap-vs-changelog separation.
    - Any test or doc cross-reference.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import subprocess
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"
CHANGELOG = REPO / "CHANGELOG.md"
CHANGELOG_D = REPO / "changelog.d"
VERSION_H = REPO / "include" / "alp" / "version.h"
PYPROJECT = REPO / "pyproject.toml"
BANNER_C = REPO / "src" / "zephyr" / "alp_banner.c"
ABI_DIR = REPO / "docs" / "abi"
ABI_SNAPSHOT_TOOL = REPO / "scripts" / "abi_snapshot.py"
EMIT_SNAPSHOT_TOOL = REPO / "scripts" / "check_emit_snapshots.py"

SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([\w.]+))?$")


def parse_version(s: str) -> tuple[int, int, int, str | None]:
    m = SEMVER_RE.match(s)
    if not m:
        raise SystemExit(f"bump_version: '{s}' is not a valid SemVer string")
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)


def read_current_version() -> str:
    text = SDK_VERSION_YAML.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\S+)", text, re.MULTILINE)
    if not m:
        raise SystemExit(f"bump_version: cannot parse {SDK_VERSION_YAML}")
    return m.group(1)


def update_sdk_version_yaml(new_version: str, dry_run: bool) -> None:
    text = SDK_VERSION_YAML.read_text(encoding="utf-8")
    new_text = re.sub(r"^version:\s*\S+", f"version: {new_version}", text, count=1, flags=re.MULTILINE)
    if new_text == text:
        # Already at the target -- report and continue, the same way
        # update_version_h() below treats an already-current version.h.
        # Refusing here blocks a legitimate cut: a version can be bumped
        # here in one PR and tagged in a later one (v0.15.0 was bumped
        # 2026-07-31 by 4d0f4aae and only ever tagged v0.15.0-rc1), and
        # this raise made the GA cut impossible without hand-editing the
        # file back first.  The duplicate-section hazard it was guarding
        # against is caught precisely in slice_changelog() instead.
        print(f"  unchanged {SDK_VERSION_YAML.relative_to(REPO)} (already at version: {new_version})")
        return
    if not dry_run:
        SDK_VERSION_YAML.write_text(new_text, encoding="utf-8", newline="")
    print(f"  updated {SDK_VERSION_YAML.relative_to(REPO)}: -> version: {new_version}")


def _pending_changelog_fragments() -> list[Path]:
    """changelog.d/*.md fragments not yet folded into CHANGELOG.md (#1395)."""
    if not CHANGELOG_D.is_dir():
        return []
    return sorted(p for p in CHANGELOG_D.glob("*.md") if p.name != "README.md")


def slice_changelog(new_version: str, dry_run: bool) -> None:
    """
    Turn `## [Unreleased] - vX candidate` into
    `## [vX] - YYYY-MM-DD` and seed a fresh empty Unreleased above it.

    No-op if Unreleased section doesn't exist (mid-cycle bumps).

    Refuses outright if a `## [vX]` section already exists, because a
    second one is unrecoverable downstream: release.yml slices the body
    by `## \\[v{VERSION}\\]` and takes the FIRST match, so the older
    section -- the one describing what actually shipped -- is silently
    orphaned from the published release notes.

    Also refuses outright if changelog.d/ still holds unfolded fragments
    (#1395): slicing now would seed the fresh [Unreleased] section BELOW
    the new [vX] heading, so every fragment authored this cycle would
    silently ship with no changelog entry at all. Run
    `python3 scripts/assemble_changelog.py` first.

    No-op (prints and returns) when `new_version` carries a SemVer
    pre-release suffix (#1902): an rc has no GA CHANGELOG section of its
    own yet, and `.github/workflows/release.yml`'s "Verify + slice
    CHANGELOG" step only ever searches for the CORE version's heading,
    falling back to `## [Unreleased]` for a pre-release tag. Slicing here
    would create a `## [vX.Y.Z-rcN]` heading that step never looks for AND
    leave `## [Unreleased]` freshly emptied -- the exact "empty body"
    failure that step's own fallback exists to avoid. The GA bump
    (`--to X.Y.Z`, no suffix) does the real slice.
    """
    _major, _minor, _patch, pre = parse_version(new_version)
    if pre:
        print(f"  skipped {CHANGELOG.relative_to(REPO)}: {new_version} is a "
              f"pre-release; '[Unreleased]' stays open until the GA bump")
        return

    pending = _pending_changelog_fragments()
    if pending:
        names = ", ".join(p.name for p in pending)
        raise SystemExit(
            f"bump_version: {len(pending)} unfolded changelog.d/ fragment(s) "
            f"remain ({names}); run `python3 scripts/assemble_changelog.py` "
            f"and commit the result before cutting v{new_version} -- slicing "
            f"now would silently drop them from the release."
        )

    text = CHANGELOG.read_text(encoding="utf-8")
    today = dt.date.today().isoformat()
    existing = re.compile(rf"^## \[v{re.escape(new_version)}\][^\n]*$", re.MULTILINE).search(text)
    if existing:
        raise SystemExit(
            f"bump_version: {CHANGELOG.relative_to(REPO)} already has a "
            f"'{existing.group(0)}' section; slicing would create a second one "
            f"and release.yml would publish only the first. Retitle or remove "
            f"the existing section before cutting v{new_version}."
        )
    pattern = re.compile(r"^## \[Unreleased\][^\n]*$", re.MULTILINE)
    m = pattern.search(text)
    if not m:
        print(f"  skipped {CHANGELOG.relative_to(REPO)}: no [Unreleased] section to slice")
        return
    fresh_unreleased = f"## [Unreleased] - v{_next_candidate(new_version)} candidate\n\n## [v{new_version}] - {today}"
    new_text = text[: m.start()] + fresh_unreleased + text[m.end():]
    if not dry_run:
        CHANGELOG.write_text(new_text, encoding="utf-8", newline="")
    print(f"  sliced {CHANGELOG.relative_to(REPO)}: [Unreleased] -> [v{new_version}] - {today}")


def _next_candidate(version: str) -> str:
    """Suggested next-version label for the new [Unreleased] section."""
    major, minor, patch, _pre = parse_version(version)
    return f"{major}.{minor + 1}.0"


def update_version_h(new_version: str, dry_run: bool) -> None:
    """Rewrite the ALP_VERSION_* macros in include/alp/version.h.

    Preserves the surrounding whitespace/alignment (the repo
    clang-format aligns consecutive macro values), substituting only
    the numeric / string values.  Keep in lockstep with
    scripts/check_version_doc_sync.py's version.h parsers.

    ALP_VERSION_MAJOR/MINOR/PATCH are always the plain core integers
    (`_pre` discarded); ALP_VERSION_STRING gets `new_version` VERBATIM,
    suffix included -- this is what makes a pre-release build report
    itself as one (#1902), e.g. `--to 1.0.0-rc1` -> `ALP_VERSION_STRING
    "1.0.0-rc1"` while MAJOR/MINOR/PATCH are still 1/0/0.
    """
    major, minor, patch, _pre = parse_version(new_version)
    text = version_h_text = VERSION_H.read_text(encoding="utf-8")
    subs = [
        (r"(#define\s+ALP_VERSION_MAJOR\s+)\d+", rf"\g<1>{major}"),
        (r"(#define\s+ALP_VERSION_MINOR\s+)\d+", rf"\g<1>{minor}"),
        (r"(#define\s+ALP_VERSION_PATCH\s+)\d+", rf"\g<1>{patch}"),
        (r'(#define\s+ALP_VERSION_STRING\s+)"[^"]*"', rf'\g<1>"{new_version}"'),
    ]
    for pat, repl in subs:
        text, n = re.subn(pat, repl, text, count=1)
        if n != 1:
            raise SystemExit(f"bump_version: pattern '{pat}' not found in "
                             f"{VERSION_H.relative_to(REPO)}")
    if text == version_h_text:
        print(f"  unchanged {VERSION_H.relative_to(REPO)} (already at {new_version})")
        return
    if not dry_run:
        VERSION_H.write_text(text, encoding="utf-8", newline="")
    print(f"  updated {VERSION_H.relative_to(REPO)}: ALP_VERSION_* -> {new_version}")


def update_banner_c(new_version: str, dry_run: bool) -> None:
    """Rewrite the sample banner line in src/zephyr/alp_banner.c's doc-comment.

    The banner *code* always prints the live ALP_VERSION_STRING at runtime;
    only this illustrative comment line can drift.  Keep in lockstep with
    scripts/check_version_doc_sync.py's check_banner_c().

    Always the CORE MAJOR.MINOR.PATCH, even when `new_version` carries a
    SemVer pre-release suffix (#1902): the sample line isn't the
    self-identification surface (ALP_VERSION_STRING is, and the banner's
    *code* prints that live), so it stays pinned to the target GA triple
    the same way pyproject.toml does below.
    """
    major, minor, patch, _pre = parse_version(new_version)
    core = f"{major}.{minor}.{patch}"
    text = BANNER_C.read_text(encoding="utf-8")
    new_text, n = re.subn(r"Alp SDK \d+\.\d+\.\d+", f"Alp SDK {core}", text, count=1)
    if n != 1:
        raise SystemExit(f"bump_version: no 'Alp SDK X.Y.Z' sample banner line in "
                         f"{BANNER_C.relative_to(REPO)}")
    if new_text == text:
        print(f"  unchanged {BANNER_C.relative_to(REPO)} (already at {core})")
        return
    if not dry_run:
        BANNER_C.write_text(new_text, encoding="utf-8", newline="")
    print(f"  updated {BANNER_C.relative_to(REPO)}: sample banner -> \"Alp SDK {core}\"")


def update_pyproject(new_version: str, dry_run: bool) -> None:
    """Rewrite the [project] version in pyproject.toml (alp-sdk-cli).

    Always the CORE MAJOR.MINOR.PATCH, even when `new_version` carries a
    SemVer pre-release suffix (#1902): PEP 440 doesn't accept a bare
    hyphenated suffix like "-rc1", and packaging metadata isn't where an rc
    identifies itself -- include/alp/version.h's ALP_VERSION_STRING and
    metadata/sdk_version.yaml's `version:` carry the full string instead.
    """
    major, minor, patch, _pre = parse_version(new_version)
    core = f"{major}.{minor}.{patch}"
    text = PYPROJECT.read_text(encoding="utf-8")
    new_text, n = re.subn(r'^version\s*=\s*"[^"]*"', f'version = "{core}"',
                          text, count=1, flags=re.MULTILINE)
    if n != 1:
        raise SystemExit(f"bump_version: no 'version = \"...\"' line in "
                         f"{PYPROJECT.relative_to(REPO)}")
    if new_text == text:
        print(f"  unchanged {PYPROJECT.relative_to(REPO)} (already at {core})")
        return
    if not dry_run:
        PYPROJECT.write_text(new_text, encoding="utf-8", newline="")
    print(f"  updated {PYPROJECT.relative_to(REPO)}: -> version = \"{core}\"")


def regenerate_abi_snapshot(new_version: str, dry_run: bool) -> None:
    major, minor, _patch, _pre = parse_version(new_version)
    snapshot_path = ABI_DIR / f"v{major}.{minor}-snapshot.json"
    cmd = [
        sys.executable,
        str(ABI_SNAPSHOT_TOOL),
        "--version",
        f"v{major}.{minor}",
        "--output",
        str(snapshot_path),
    ]
    if dry_run:
        print(f"  would run: {' '.join(cmd)}")
        return
    subprocess.check_call(cmd)
    print(f"  regenerated {snapshot_path.relative_to(REPO)}")


def regenerate_emit_snapshots(dry_run: bool) -> None:
    """Rewrite the `--emit` goldens under tests/fixtures/emit-snapshots/ (#1461).

    Two paths carry the version into these goldens: build-plan's
    `sdkVersion` field (read straight from metadata/sdk_version.yaml by
    scripts/alp_orchestrate/buildplan.py::_sdk_version()) and a released
    scaffold's README doc links (pinned to v<version> by
    scripts/alp_template.py::_docs_ref() once status: released). Without
    this, every version bump reds check_emit_snapshots.py until a human
    runs `--update` by hand.
    """
    cmd = [sys.executable, str(EMIT_SNAPSHOT_TOOL), "--update"]
    if dry_run:
        print(f"  would run: {' '.join(cmd)}")
        return
    subprocess.check_call(cmd)
    print("  regenerated tests/fixtures/emit-snapshots/ (--emit goldens)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--to", required=True, help="Target version (SemVer, e.g. 1.0.0)")
    ap.add_argument("--dry-run", action="store_true", help="Show what would change without writing")
    args = ap.parse_args()

    parse_version(args.to)  # validates SemVer shape
    current = read_current_version()

    print(f"bump_version: {current} -> {args.to}" + ("  [dry run]" if args.dry_run else ""))
    print()
    update_sdk_version_yaml(args.to, args.dry_run)
    slice_changelog(args.to, args.dry_run)
    update_version_h(args.to, args.dry_run)
    update_pyproject(args.to, args.dry_run)
    update_banner_c(args.to, args.dry_run)
    regenerate_abi_snapshot(args.to, args.dry_run)
    regenerate_emit_snapshots(args.dry_run)
    print()
    print("Next steps:")
    print("  git diff --stat")
    print("  git add -A")
    print(f'  git commit -m "chore: bump version to {args.to}"')
    print(f"  git tag -s v{args.to} -m 'v{args.to}'")
    print(f"  # push when ready: git push origin main --tags")
    return 0


if __name__ == "__main__":
    sys.exit(main())
