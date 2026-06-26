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
    README.md                       -- the current-state version labels (e.g.
                                       "Partially silicon-verified (`vX.Y`)", "vX.Y ramp");
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
README = REPO / "README.md"
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


def update_readme_version_labels(current: str, new_version: str, dry_run: bool) -> None:
    """Bump the README's current-state version labels to the new MAJOR.MINOR.

    Touches ONLY the anchored current-state labels (the same ones
    scripts/check_version_doc_sync.py enforces) -- the historical "landed in
    vX" / "from vX onward" references are correct and left untouched.  Keep
    these substitutions in lockstep with that check's _ANCHORS.
    """
    old_mm = ".".join(current.split(".")[:2])
    new_mm = ".".join(new_version.split(".")[:2])
    if old_mm == new_mm:
        return
    text = README.read_text(encoding="utf-8")
    subs = [
        (rf"(Partially silicon-verified \(`v){re.escape(old_mm)}(`\))", rf"\g<1>{new_mm}\g<2>"),
        (rf"(\*\*v){re.escape(old_mm)}( ramp — paper-correct)", rf"\g<1>{new_mm}\g<2>"),
    ]
    total = 0
    for pat, repl in subs:
        text, n = re.subn(pat, repl, text)
        total += n
    if total == 0:
        print(f"  skipped {README.relative_to(REPO)}: no current-state v{old_mm} labels found")
        return
    if not dry_run:
        README.write_text(text, encoding="utf-8")
    print(f"  updated {README.relative_to(REPO)}: {total} current-state label(s) "
          f"v{old_mm} -> v{new_mm}")


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
    update_readme_version_labels(current, args.to, args.dry_run)
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
