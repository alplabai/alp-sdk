#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Lint board.yaml fragments embedded in Markdown documentation.

Walks every tracked *.md file under the repo root, extracts ```yaml
fenced blocks that *look like* full board.yaml documents (top-level
`som:` + `cores:` markers), and validates each fragment against the
corresponding schema in `metadata/schemas/`.

Catches the failure mode where a doc shows a board.yaml snippet that
no longer matches the schema after a field is added / removed --
e.g. README.md quick-start carrying `inference: { backend: cpu }`
after commit a3cd4fd removed it.

Exit codes:
  0  every fragment validates clean (or no fragments found)
  1  one or more fragments failed schema validation
  2  invocation error (missing schema file, missing deps, etc.)

Usage:
  python3 scripts/lint_doc_yaml_fragments.py                  # walk default scope
  python3 scripts/lint_doc_yaml_fragments.py --root docs       # restrict scope
  python3 scripts/lint_doc_yaml_fragments.py --path README.md  # single file

The script is intentionally narrow: it ONLY checks JSON Schema
validity, not preset / hw_rev / capability cross-checks (those
belong in `validate_board_yaml.py` and require a live SoM SKU on
disk).  A docs fragment that names a hypothetical SKU still
passes here as long as its shape obeys the schema.

CI hook: invoked from .github/workflows/pr-metadata-validate.yml.
Local invocation: stage 'doc-yaml-fragments' in scripts/test-all.sh.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover -- environmental
    sys.exit("lint_doc_yaml_fragments: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover -- environmental
    sys.exit("lint_doc_yaml_fragments: jsonschema is required.  Install via `pip install jsonschema`.")


REPO = Path(__file__).resolve().parent.parent

# Directories never scanned -- vendored upstreams, agent worktrees,
# build outputs, and the design-spec drawer that intentionally carries
# pre-cleanup examples for historical reference.
DEFAULT_EXCLUDES: tuple[str, ...] = (
    ".claude",
    ".git",
    "build",
    "node_modules",
    "vendors",
    "docs/superpowers/specs",
    "docs/superpowers/plans",
    "metadata/library-profiles/cmsis-dsp",
)

# Fenced-block extractor.  Captures content between ```yaml (or ```yml)
# and the matching ``` terminator.  We allow optional whitespace after
# the language tag because some markdown linters insert one.
_FENCE_RE = re.compile(
    r"^[ \t]*```(?:yaml|yml)[ \t]*\n(.*?)^[ \t]*```[ \t]*$",
    flags=re.MULTILINE | re.DOTALL,
)

# Heuristic: a fragment is treated as a board.yaml document when it
# has `som:` plus at least one of `cores:` (the per-core mapping the
# schema requires) so that plain library-profile / soc-spec /
# hw-revisions snippets that share the same fenced-block syntax
# don't get force-validated against the board.yaml schema.
_BOARD_YAML_MARKERS: frozenset[str] = frozenset({"som", "cores"})


@dataclass(frozen=True)
class Fragment:
    """One ```yaml fenced block in a markdown file."""
    path: Path                  # repo-relative path to the .md file
    line: int                   # 1-based line number of the opening fence
    body: str                   # raw YAML text inside the fence

    def __str__(self) -> str:
        return f"{self.path.as_posix()}:{self.line}"


@dataclass(frozen=True)
class LintResult:
    """One validated fragment + its outcome."""
    fragment: Fragment
    ok: bool
    error: str | None       # populated when ok=False


def _is_excluded(path: Path, excludes: tuple[str, ...]) -> bool:
    """Return True if `path` (repo-relative) starts with any excluded
    prefix.  Path components are compared, not raw string prefixes,
    so `vendors` excludes `vendors/foo/bar.md` but not `vendors_oss/`."""
    parts = path.parts
    for ex in excludes:
        ex_parts = tuple(p for p in ex.split("/") if p)
        if len(ex_parts) <= len(parts) and parts[:len(ex_parts)] == ex_parts:
            return True
    return False


def _git_tracked_markdown(root: Path) -> list[Path] | None:
    """Fast path: git-tracked ``*.md`` under `root` via ``git ls-files``.

    Returns None when `root` is not a git work tree or git is unavailable, so
    callers fall back to a filesystem walk."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", "*.md"],
            capture_output=True, timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    rels = proc.stdout.decode("utf-8", "surrogateescape").split("\0")
    return [root / r for r in rels if r]


def discover_markdown(root: Path, excludes: tuple[str, ...]) -> list[Path]:
    """Return all .md files under `root` outside `excludes`.

    Prefers ``git ls-files`` -- the linter only checks *tracked* docs, and
    walking the working tree (especially ``.git/`` + ``vendors/``) over a
    9p/WSL mount took >10 min, which looked like the whole pytest suite
    hanging; reading the git index is ~instant.  Falls back to an ``os.walk``
    that prunes excluded dirs in place when `root` is not a git work tree with
    tracked markdown (e.g. the unit tests' tmp dirs).  Either way the result is
    filtered through `_is_excluded` so the exclude semantics are identical."""
    tracked = _git_tracked_markdown(root)
    if tracked:
        candidates: list[Path] = tracked
    else:
        candidates = []
        for dirpath, dirnames, filenames in os.walk(root):
            here = Path(dirpath)
            # Drop excluded subdirs in place so os.walk won't descend into them.
            dirnames[:] = [
                d for d in dirnames
                if not _is_excluded((here / d).relative_to(root), excludes)
            ]
            candidates.extend(here / fn for fn in filenames if fn.endswith(".md"))
    result = [p for p in candidates if not _is_excluded(p.relative_to(root), excludes)]
    result.sort()
    return result


def extract_fragments(md_path: Path) -> list[Fragment]:
    """Return every ```yaml fenced block in `md_path`."""
    text = md_path.read_text(encoding="utf-8")
    out: list[Fragment] = []
    for match in _FENCE_RE.finditer(text):
        body = match.group(1)
        # Line number of the opening fence (1-based).
        line = text.count("\n", 0, match.start()) + 1
        out.append(Fragment(path=md_path, line=line, body=body))
    return out


def parse_fragment(fragment: Fragment) -> dict[str, Any] | None:
    """YAML-parse one fragment.  Returns None for fragments that are
    not board.yaml documents (lack the `som:` + `cores:` markers) or
    that fail to parse.  The latter is intentionally non-fatal:
    fragments are often examples mid-thought, and the linter only
    enforces schema-correctness on fragments that claim to be
    full board.yaml documents."""
    try:
        data = yaml.safe_load(fragment.body)
    except yaml.YAMLError:
        return None
    if not isinstance(data, dict):
        return None
    if not _BOARD_YAML_MARKERS.issubset(data.keys()):
        return None
    return data


def _load_schema(schemas_dir: Path) -> dict[str, Any] | None:
    """Return the board.yaml schema, or None when the file is missing."""
    path = schemas_dir / "board.schema.json"
    if not path.is_file():
        return None
    return json.loads(path.read_text(encoding="utf-8"))


def validate_fragment(
    fragment: Fragment,
    data: dict[str, Any],
    schemas_dir: Path,
) -> LintResult:
    """Schema-validate one parsed fragment."""
    schema = _load_schema(schemas_dir)
    if schema is None:
        # Schema missing on disk -- not the linter's job to police the
        # SDK layout; the loader will reject builds without it.
        return LintResult(fragment=fragment, ok=True, error=None)

    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(
        validator.iter_errors(data),
        key=lambda e: list(e.absolute_path),
    )
    if not errors:
        return LintResult(fragment=fragment, ok=True, error=None)

    bits: list[str] = []
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        bits.append(f"  {loc}: {err.message}")
    return LintResult(fragment=fragment, ok=False, error="\n".join(bits))


def lint(
    paths: list[Path],
    schemas_dir: Path,
) -> tuple[list[LintResult], int]:
    """Lint a list of *.md files.  Returns (results, num_fragments_checked).
    `results` includes both pass and fail outcomes; fragments that
    aren't board.yaml documents are silently skipped."""
    results: list[LintResult] = []
    checked = 0
    for md_path in paths:
        fragments = extract_fragments(md_path)
        for fragment in fragments:
            parsed = parse_fragment(fragment)
            if parsed is None:
                continue
            checked += 1
            results.append(validate_fragment(fragment, parsed, schemas_dir))
    return results, checked


def _print_report(results: list[LintResult], checked: int) -> int:
    """Print a human-readable report.  Returns process exit code."""
    failures = [r for r in results if not r.ok]
    if checked == 0:
        print("doc-yaml-lint: no board.yaml fragments found")
        return 0
    for r in results:
        marker = "OK  " if r.ok else "FAIL"
        print(f"{marker} {r.fragment}")
        if not r.ok and r.error:
            print(r.error)
    print()
    if failures:
        print(f"doc-yaml-lint: {len(failures)} of {checked} fragment(s) failed")
        return 1
    print(f"doc-yaml-lint: {checked} fragment(s) clean")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Lint board.yaml fragments embedded in markdown docs.",
    )
    parser.add_argument(
        "--root", type=Path, default=REPO,
        help="Directory to walk for *.md files (default: repo root).",
    )
    parser.add_argument(
        "--path", type=Path, action="append", default=None,
        help="Lint a specific .md file (may be repeated).  Overrides --root.",
    )
    parser.add_argument(
        "--schemas-dir", type=Path,
        default=REPO / "metadata" / "schemas",
        help="Directory containing board-config-vN.schema.json files.",
    )
    parser.add_argument(
        "--exclude", action="append", default=None,
        help="Path prefix (repo-relative) to exclude.  May be repeated.  "
             "If omitted, the built-in default exclude set is used.",
    )
    args = parser.parse_args()

    if not args.schemas_dir.is_dir():
        print(f"lint_doc_yaml_fragments: schemas dir not found: {args.schemas_dir}",
              file=sys.stderr)
        return 2

    if args.path:
        paths = [p.resolve() for p in args.path]
        for p in paths:
            if not p.is_file():
                print(f"lint_doc_yaml_fragments: not a file: {p}", file=sys.stderr)
                return 2
    else:
        excludes = tuple(args.exclude) if args.exclude is not None else DEFAULT_EXCLUDES
        paths = discover_markdown(args.root.resolve(), excludes)

    results, checked = lint(paths, args.schemas_dir.resolve())
    return _print_report(results, checked)


if __name__ == "__main__":
    sys.exit(main())
