#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
One-command release-prep tool.

Bumps the SDK version in every place that tracks it, regenerates the
ABI snapshot for the new version, and (optionally) creates the git
tag.  Doesn't push -- the operator does that explicitly so a bad bump
can be undone locally.

Workflow:

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

What this touches:

    metadata/sdk_version.yaml       -- the declared version.
    CHANGELOG.md                    -- slice [Unreleased] into the new version section.
    docs/abi/v<MAJOR.MINOR>-snapshot.json  -- regenerated.
    include/alp/version.h           -- ALP_VERSION_MAJOR/MINOR/PATCH +
                                       ALP_VERSION_STRING macros;
                                       enforced by scripts/check_version_doc_sync.py.
    pyproject.toml                  -- the alp-sdk-cli [project] version;
                                       enforced by scripts/check_version_doc_sync.py.

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
VERSION_H = REPO / "include" / "alp" / "version.h"
PYPROJECT = REPO / "pyproject.toml"
ABI_DIR = REPO / "docs" / "abi"
ABI_SNAPSHOT_TOOL = REPO / "scripts" / "abi_snapshot.py"

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
        raise SystemExit("bump_version: no change to sdk_version.yaml (already at target?)")
    if not dry_run:
        SDK_VERSION_YAML.write_text(new_text, encoding="utf-8")
    print(f"  updated {SDK_VERSION_YAML.relative_to(REPO)}: -> version: {new_version}")


def slice_changelog(new_version: str, dry_run: bool) -> None:
    """
    Turn `## [Unreleased] - vX candidate` into
    `## [vX] - YYYY-MM-DD` and seed a fresh empty Unreleased above it.

    No-op if Unreleased section doesn't exist (mid-cycle bumps).
    """
    text = CHANGELOG.read_text(encoding="utf-8")
    today = dt.date.today().isoformat()
    pattern = re.compile(r"^## \[Unreleased\][^\n]*$", re.MULTILINE)
    m = pattern.search(text)
    if not m:
        print(f"  skipped {CHANGELOG.relative_to(REPO)}: no [Unreleased] section to slice")
        return
    sliced_header = f"## [v{new_version}] - {today}"
    fresh_unreleased = f"## [Unreleased] - v{_next_candidate(new_version)} candidate\n\n## [v{new_version}] - {today}"
    new_text = text[: m.start()] + fresh_unreleased + text[m.end():]
    if not dry_run:
        CHANGELOG.write_text(new_text, encoding="utf-8")
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
        VERSION_H.write_text(text, encoding="utf-8")
    print(f"  updated {VERSION_H.relative_to(REPO)}: ALP_VERSION_* -> {new_version}")


def update_pyproject(new_version: str, dry_run: bool) -> None:
    """Rewrite the [project] version in pyproject.toml (alp-sdk-cli)."""
    text = PYPROJECT.read_text(encoding="utf-8")
    new_text, n = re.subn(r'^version\s*=\s*"[^"]*"', f'version = "{new_version}"',
                          text, count=1, flags=re.MULTILINE)
    if n != 1:
        raise SystemExit(f"bump_version: no 'version = \"...\"' line in "
                         f"{PYPROJECT.relative_to(REPO)}")
    if new_text == text:
        print(f"  unchanged {PYPROJECT.relative_to(REPO)} (already at {new_version})")
        return
    if not dry_run:
        PYPROJECT.write_text(new_text, encoding="utf-8")
    print(f"  updated {PYPROJECT.relative_to(REPO)}: -> version = \"{new_version}\"")


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
    regenerate_abi_snapshot(args.to, args.dry_run)
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
