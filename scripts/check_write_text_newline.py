#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Guard: every scripts/ write_text() of a repo-tree text artifact passes
newline="".

Path.write_text() translates every '\\n' to os.linesep on write. On a
Windows host that silently rewrites the whole file to CRLF; .gitattributes
normalizes it back to LF on `git add`, so the bug never reaches a commit and
never reds CI -- it just leaves the regenerated file permanently
whole-file-dirty in every working tree, burying the one line that actually
changed underneath a wall of line-ending noise. Fix: newline="" (write LF
verbatim; the caller's own text already uses '\\n').

A genuine tempfile/build/scratch/out-of-tree writer is exempt per call, not
per file: mark the call itself (or the line directly above it) with

    # write-text-newline-exempt: <reason>

A bare marker with no reason is a defect in the marker, not a pass -- it is
reported on its own. A `newline=` keyword must be the literal `""` or
`"\\n"`; `newline=None` -- the CRLF-translating default -- any other string
literal (`"\\r\\n"`, `"\\r"`), or any non-literal value (a variable, an
expression, an f-string) is flagged too.

Markers and calls are resolved from real `tokenize.COMMENT` tokens, not raw
line text, so a marker spelled inside a string literal or a docstring never
counts, and a marker trailing one call's line never leaks onto an unmarked
call on the next line.

Scope limit: this only matches the `.write_text()` attribute call. A repo-
tree writer using `open(path, "w")` (which translates '\\n' to os.linesep
identically) is not yet checked here.
"""
from __future__ import annotations

import argparse
import ast
import io
import sys
import tokenize
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_MARKER = "write-text-newline-exempt"


def _comment_map(text: str) -> dict[int, str]:
    """Map physical line number -> full comment text (including '#'), built
    from real `tokenize.COMMENT` tokens only. A marker token that appears
    inside a string literal or docstring is invisible here, and the line
    numbers line up with `ast`'s (both count only '\\n' as a line break --
    unlike `str.splitlines()`, which also breaks on FF/VT/NEL/U+2028/U+2029)."""
    comments: dict[int, str] = {}
    try:
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                comments[tok.start[0]] = tok.string
    except (tokenize.TokenError, IndentationError):
        pass
    return comments


def _marker_reason(
    node: ast.Call,
    comments: dict[int, str],
    spans: list[tuple[int, int, int]],
) -> str | None:
    """Resolve the write-text-newline-exempt marker for one write_text()
    call.

    A marker comment binds to this call only if its line is one of the
    call's own physical lines (`node.lineno..node.end_lineno`) or the line
    directly above (`node.lineno - 1`), AND that line does not fall inside
    the span of some OTHER write_text() call in the file -- otherwise a
    marker trailing call A's line would also exempt an unmarked call B
    starting on the next line (B's "line above" is A's own line).

    Every candidate line is scanned and a reasoned marker wins over a bare
    one wherever it appears, so a stray bare marker elsewhere in the window
    doesn't shadow a correctly-reasoned one covering this call.

    Returns the stripped reason string ("" for a bare marker), or None if
    no marker binds to this call at all.
    """
    end = node.end_lineno or node.lineno
    candidates = list(range(node.lineno, end + 1))
    if node.lineno - 1 >= 1:
        candidates.append(node.lineno - 1)

    found_bare = False
    for lineno in candidates:
        comment = comments.get(lineno)
        if comment is None or _MARKER not in comment:
            continue
        if any(
            other_id != id(node) and lo <= lineno <= hi
            for other_id, lo, hi in spans
        ):
            continue  # this line belongs to some OTHER call's own span
        idx = comment.find(_MARKER)
        rest = comment[idx + len(_MARKER):].lstrip()
        reason = rest[1:].strip() if rest.startswith(":") else ""
        if reason:
            return reason
        found_bare = True
    return "" if found_bare else None


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    scripts_dir = root / "scripts"
    for path in sorted(scripts_dir.rglob("*.py")):
        rel = path.relative_to(scripts_dir).as_posix()
        try:
            text = path.read_text(encoding="utf-8")
            tree = ast.parse(text, filename=str(path))
        except (SyntaxError, UnicodeDecodeError):
            continue

        calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "write_text"
        ]
        if not calls:
            continue

        comments = _comment_map(text)
        spans = [(id(c), c.lineno, c.end_lineno or c.lineno) for c in calls]

        for node in calls:
            marker = _marker_reason(node, comments, spans)
            if marker is not None:
                if marker:
                    continue
                problems.append(
                    f"scripts/{rel}:{node.lineno}: write-text-newline-exempt "
                    f"marker needs a reason -- "
                    f'"# write-text-newline-exempt: <reason>"'
                )
                continue

            newline_kw = next(
                (kw for kw in node.keywords if kw.arg == "newline"), None
            )
            if newline_kw is None:
                problems.append(
                    f"scripts/{rel}:{node.lineno}: write_text() without "
                    f'newline="" -- CRLF-on-Windows working-tree churn '
                    f"(see scripts/abi_snapshot.py for the rationale; if "
                    f"this path never lands in the repo tree, mark it "
                    f'"# write-text-newline-exempt: <reason>" instead)'
                )
            elif not (
                isinstance(newline_kw.value, ast.Constant)
                and isinstance(newline_kw.value.value, str)
            ):
                problems.append(
                    f"scripts/{rel}:{node.lineno}: write_text() newline=... "
                    f"is not a string literal -- newline=None is the "
                    f"CRLF-translating default and silently defeats the "
                    f'guard; pass a literal newline="" (or newline="\\n"), '
                    f'or mark the call "# write-text-newline-exempt: '
                    f'<reason>" if this path never lands in the repo tree'
                )
            elif newline_kw.value.value not in ("", "\n"):
                problems.append(
                    f"scripts/{rel}:{node.lineno}: write_text() "
                    f"newline={newline_kw.value.value!r} is not \"\" or "
                    f'"\\n" -- that writes a literal line ending '
                    f"(CRLF-on-disk for \"\\r\\n\") instead of the "
                    f"platform default, the exact class of bug this gate "
                    f'exists to stop; pass newline="" (or newline="\\n"), '
                    f'or mark the call "# write-text-newline-exempt: '
                    f'<reason>" if this path never lands in the repo tree'
                )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repo root to scan (default: the real repo)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"\ncheck_write_text_newline: {len(problems)} site(s) need "
              f'newline="".', file=sys.stderr)
        return 1
    print("OK: every scripts/ write_text() call is newline-safe.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
