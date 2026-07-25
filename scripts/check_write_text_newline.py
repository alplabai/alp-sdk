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

A handful of writers are intentionally exempt: they write to a tempfile/
build/scratch directory or an out-of-tree user-supplied --output path that
never lands in the repo tree, so LF-vs-CRLF there is not this repo's
problem. Judged by hand (not detectable from syntax alone) and listed
whole-file in _EXEMPT below.
"""
from __future__ import annotations

import ast
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_EXEMPT = {
    "alp_cli/emit.py",
    "alp_cli/init.py",
    "alp_mcp/server.py",
    "alp_orchestrate/kconfig_symbols.py",
    "gen_sbom.py",
    "gen_portability_matrix.py",  # newline="" landing separately, see #939
    "kconfig/alp_kconfig_dump.py",
    "provision_som.py",
    "west_commands/runners/alif_flash.py",
    "extract_pdf.py",
    "flash_backends/swd_probe.py",
}


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    scripts_dir = root / "scripts"
    for path in sorted(scripts_dir.rglob("*.py")):
        rel = path.relative_to(scripts_dir).as_posix()
        if rel in _EXEMPT or path.name == "check_write_text_newline.py":
            continue
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except (SyntaxError, UnicodeDecodeError):
            continue
        for node in ast.walk(tree):
            is_write_text = (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == "write_text"
            )
            if is_write_text and not any(kw.arg == "newline" for kw in node.keywords):
                problems.append(
                    f"scripts/{rel}:{node.lineno}: write_text() without "
                    f'newline="" -- CRLF-on-Windows working-tree churn '
                    f"(see scripts/abi_snapshot.py for the rationale; if "
                    f"this path never lands in the repo tree, add "
                    f"scripts/{rel} to _EXEMPT instead)"
                )
    return problems


def main() -> int:
    problems = find_problems(REPO)
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
