#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Decide whether a change set can affect the native_sim twister run.

Twister is the one expensive gate (7-34 min of a local test-all.sh run,
3-17 min per CI shard), and many changes -- a changelog fragment, a doc, a
Yocto bbappend, a pytest gate, an AEN-only bench example -- cannot change a
single native_sim build input.  This script maps the changed paths (vs a
base revision) to one answer:

    full  run the whole twister suite (the default, and every fail-safe)
    skip  every changed path is PROVABLY outside the native_sim build inputs

A path is "outside" only if one of these rules proves it:

  1. It is under an explicit prose/CI/other-OS prefix (SKIP_PREFIXES), or
     is a Markdown file.
  2. It is a scripts/ file outside the build-time closure: every scripts/
     path a build file (CMakeLists.txt, *.cmake, Kconfig*, *.conf,
     *.overlay, testcase/sample.yaml, module.yml) names outside a comment,
     plus every scripts/ module those Python files name, transitively.
     Computed from the tree on every run, so a new CMake-time script widens
     the closure by itself.
  3. It is inside a twister suite directory (examples/**, tests/{unit,
     zephyr,console}/** holding a testcase.yaml/sample.yaml) whose every
     scenario pins platform_allow to non-native_sim boards, has no nested
     suite, and whose directory name no build or C file outside it mentions
     -- so nothing native_sim builds can include it.

Anything else -- a new top-level directory, CMake, Kconfig, a header,
metadata/ (read at CMake time by scripts/alp_project.py) -- means ``full``.
So do a failed git call, an empty diff, a base revision that does not
resolve, and a missing PyYAML.

Used by scripts/test-all.sh (local; ``--full`` and ``--target main`` bypass
it) and .github/workflows/pr-twister.yml (pull_request + merge_group only;
push to dev/main, the nightly schedule and workflow_dispatch always run
full).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Rule 1.  A trailing "/" is a directory prefix, anything else an exact path.
SKIP_PREFIXES = (
    "changelog.d/",          # release-note fragments
    "CHANGELOG.md",
    "docs/",                 # prose, ADRs, ABI snapshots (gate inputs only)
    "meta-alp-sdk/",         # Yocto layer; bitbake-only
    "tests/scripts/",        # pytest gates
    "tests/hil/",            # hardware-in-the-loop specs (bench only)
    "tests/yocto/",          # Yocto-side tests
    "tests/parity/",         # seam-1 comparator (pytest)
    ".github/",              # CI config -- except the files in FULL_EXACT
    ".vscode/",
    ".superpowers/",
    ".editorconfig",
    ".git-blame-ignore-revs",
    ".pre-commit-config.yaml",
    "CODEOWNERS",
    "LICENSE",
    "NOTICE",
    "llms.txt",
)

# These define HOW twister runs, so they force full even where a rule above
# would skip them.
FULL_EXACT = frozenset({
    ".github/workflows/pr-twister.yml",
    "scripts/ci/apt-bounded.sh",
    "scripts/test-all.sh",
    "scripts/select_checks.py",
})

_MARKDOWN = re.compile(r"\.md$", re.IGNORECASE)
_BUILD_FILE = re.compile(
    r"(^|/)(CMakeLists\.txt|Kconfig[^/]*|testcase\.yaml|sample\.yaml|module\.yml)$"
    r"|\.(cmake|conf|overlay)$"
)
# Everything a build can #include / rsource / file(GLOB) -- scanned for
# suite-directory references (rule 3).
_SOURCE_FILE = re.compile(r"\.(c|h|cpp|hpp|cc|S|ld|dts|dtsi|txt|cmake|conf|overlay|ya?ml)$"
                          r"|(^|/)Kconfig[^/]*$")
_BUILD_ROOTS = ("zephyr/", "examples/", "tests/", "cmake/", "src/", "chips/", "vendors/",
                "include/", "include-testing/", "blocks/", "firmware/", "CMakeLists.txt")
_SUITE_ROOTS = ("examples/", "tests/unit/", "tests/zephyr/", "tests/console/")
_SUITE_YAML = ("testcase.yaml", "sample.yaml")
# C-preprocessed files: `#` is a directive there, comments are /* */ and //.
_C_FAMILY = re.compile(r"\.(c|h|cpp|hpp|cc|S|ld|dts|dtsi|overlay)$")
_C_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
# `//` only after start-of-line or whitespace, so "http://..." survives.
_C_LINE_COMMENT = re.compile(r"(^|\s)//[^\n]*")
# CMake/Kconfig/conf/YAML: strip only FULL-line `#` comments -- a `#` later
# in a line may sit inside a string, and stripping a real reference would
# err in the unsafe direction.
_HASH_COMMENT_LINE = re.compile(r"^\s*#[^\n]*", re.MULTILINE)
_SCRIPTS_REF = re.compile(r"scripts/[A-Za-z0-9_./-]+")
_WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_PRUNE = {".git", "__pycache__", "node_modules"}


def _walk(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in _PRUNE and not d.startswith(("twister-out", "build", ".venv"))]
        for name in filenames:
            path = Path(dirpath, name)
            yield path, path.relative_to(root).as_posix()


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _uncommented(rel: str, text: str) -> str:
    """`text` minus comments, erring towards keeping text (the safe side)."""
    if _C_FAMILY.search(rel):
        return _C_LINE_COMMENT.sub(r"\1", _C_BLOCK_COMMENT.sub(" ", text))
    if rel.rsplit("/", 1)[-1] in _SUITE_YAML:
        # A suite's prose `description:` is not a build input; everything
        # else (extra_args, extra_conf_files, ...) is kept.
        try:
            import yaml
            return yaml.safe_dump(_drop_descriptions(yaml.safe_load(text)))
        except Exception:  # noqa: BLE001 - unparsable/no PyYAML: keep it all
            pass
    return _HASH_COMMENT_LINE.sub("", text)


def _drop_descriptions(node):
    if isinstance(node, dict):
        return {k: _drop_descriptions(v) for k, v in node.items() if k != "description"}
    if isinstance(node, list):
        return [_drop_descriptions(v) for v in node]
    return node


class Tree:
    """Lazily computed facts about one checkout."""

    def __init__(self, root: Path):
        self.root = root
        self._closure: set[str] | None = None
        self._sources: dict[str, str] | None = None

    def sources(self) -> dict[str, str]:
        if self._sources is None:
            # Comments stripped: a comment can name a script or a suite
            # without the build ever reading it.
            self._sources = {rel: _uncommented(rel, _read(path))
                             for path, rel in _walk(self.root)
                             if rel.startswith(_BUILD_ROOTS) and _SOURCE_FILE.search(rel)}
        return self._sources

    def closure(self) -> set[str]:
        """Repo-relative scripts/ paths a native_sim build can execute or read."""
        if self._closure is not None:
            return self._closure
        root = self.root
        todo: list[str] = []
        for rel, text in self.sources().items():
            if not _BUILD_FILE.search(rel):
                continue
            for ref in _SCRIPTS_REF.findall(text):
                ref = ref.rstrip("./")
                target = root / ref
                if target.is_dir():
                    todo.extend(p.relative_to(root).as_posix() for p in target.rglob("*")
                                if p.is_file())
                elif target.exists():
                    todo.append(ref)

        names: dict[str, list[str]] = {}
        scripts = root / "scripts"
        if scripts.is_dir():
            for entry in scripts.iterdir():
                if entry.suffix == ".py":
                    names[entry.stem] = [f"scripts/{entry.name}"]
                elif entry.is_dir() and (entry / "__init__.py").exists():
                    names[entry.name] = [p.relative_to(root).as_posix()
                                         for p in entry.rglob("*.py")]

        closure: set[str] = set()
        while todo:
            rel = todo.pop()
            if rel in closure:
                continue
            closure.add(rel)
            if rel.endswith(".py"):
                # Over-approximate on purpose: any identifier equal to a
                # scripts/ module name -- an import, an importlib string, a
                # subprocess argv, even a comment -- pulls that module in.
                # A too-big closure only makes twister run more often.
                for word in set(_WORD.findall(_read(root / rel))):
                    todo.extend(names.get(word, ()))
        self._closure = closure
        return closure

    def suite_dir(self, path: str) -> str | None:
        """Nearest ancestor directory of `path` holding a twister suite yaml."""
        if not path.startswith(_SUITE_ROOTS):
            return None
        parts = path.split("/")[:-1]
        while len(parts) > 2:
            d = "/".join(parts)
            if any((self.root / d / y).is_file() for y in _SUITE_YAML):
                return d
            parts.pop()
        return None

    def suite_skips_native_sim(self, d: str) -> str | None:
        """Why suite dir `d` cannot touch a native_sim build, or None."""
        try:
            import yaml
        except ImportError:
            return None
        allows = []
        for y in _SUITE_YAML:
            f = self.root / d / y
            if not f.is_file():
                continue
            try:
                doc = yaml.safe_load(_read(f)) or {}
            except yaml.YAMLError:
                return None
            common = _as_list((doc.get("common") or {}).get("platform_allow"))
            scenarios = doc.get("tests") or {}
            if not isinstance(scenarios, dict) or not scenarios:
                return None
            for body in scenarios.values():
                allow = common + _as_list((body or {}).get("platform_allow"))
                if not allow or any("native" in str(p) for p in allow):
                    return None
                allows.extend(allow)
        if not allows:
            return None
        prefix = d + "/"
        for rel in self.sources():
            if rel.startswith(prefix) and rel.rsplit("/", 1)[-1] in _SUITE_YAML \
                    and rel.count("/") > d.count("/") + 1:
                return None  # nested suite: may build files from this dir
        name = d.rsplit("/", 1)[-1]
        for rel, text in self.sources().items():
            if not rel.startswith(prefix) and name in text:
                return None  # something outside may include this suite's files
        return f"suite {d} is platform_allow-pinned off native_sim"


def _as_list(value) -> list:
    if value is None:
        return []
    if isinstance(value, str):
        return value.split()
    return list(value)


def classify(paths: list[str], root: Path = REPO,
             tree: "Tree | None" = None) -> tuple[str, list[str]]:
    """Return ("full"|"skip", reasons).  Reasons name the deciding paths."""
    if not paths:
        return "full", ["empty change set (fail-safe: nothing to prove)"]
    tree = tree or Tree(root)
    reasons = []
    for path in sorted(set(paths)):
        if path in FULL_EXACT:
            return "full", [f"{path}: defines how twister runs"]
        if path.startswith("scripts/"):
            if path in tree.closure():
                return "full", [f"{path}: in the native_sim build-time closure"]
            reasons.append(f"{path}: scripts/ file outside the build-time closure")
            continue
        if _MARKDOWN.search(path):
            reasons.append(f"{path}: Markdown")
            continue
        if any(path == p or (p.endswith("/") and path.startswith(p)) for p in SKIP_PREFIXES):
            reasons.append(f"{path}: outside the twister inputs")
            continue
        d = tree.suite_dir(path)
        why = tree.suite_skips_native_sim(d) if d else None
        if why:
            reasons.append(f"{path}: {why}")
            continue
        return "full", [f"{path}: can affect native_sim builds (or unknown)"]
    return "skip", reasons


def changed_paths(base: str, head: str | None, worktree: bool, root: Path) -> list[str]:
    """Paths changed from `base` to `head` (or from merge-base(base, HEAD)).

    Any git failure raises; main() turns that into ``full``.
    """
    def git(*args: str) -> list[str]:
        out = subprocess.run(["git", "-C", str(root), *args], check=True,
                             capture_output=True, text=True, encoding="utf-8").stdout
        return [line for line in out.splitlines() if line]

    if head:
        files = git("diff", "--no-renames", "--name-only", base, head)
    else:
        merge_base = git("merge-base", base, "HEAD")[0]
        files = git("diff", "--no-renames", "--name-only", merge_base, "HEAD")
    if worktree:
        files += git("diff", "--no-renames", "--name-only", "HEAD")
        files += git("ls-files", "--others", "--exclude-standard")
    return files


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", help="base revision (diffed from its merge base with HEAD "
                                   "unless --head is given)")
    ap.add_argument("--head", help="head revision; diff base..head directly (CI)")
    ap.add_argument("--worktree", action="store_true",
                    help="also count uncommitted and untracked files (local runs)")
    ap.add_argument("--files", nargs="*", help="classify these paths instead of asking git")
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--github-output", action="store_true",
                    help="also append twister=full|skip to $GITHUB_OUTPUT")
    args = ap.parse_args(argv)

    if args.files is None and not args.base:
        decision, reasons = "full", ["no --base or --files given"]
    else:
        try:
            paths = args.files if args.files is not None else \
                changed_paths(args.base, args.head, args.worktree, args.root)
            decision, reasons = classify(paths, args.root)
        except (subprocess.CalledProcessError, OSError, IndexError) as exc:
            decision, reasons = "full", [f"git failed ({exc}); fail-safe"]

    for reason in reasons[:40]:
        print(f"  {reason}", file=sys.stderr)
    if len(reasons) > 40:
        print(f"  ... and {len(reasons) - 40} more", file=sys.stderr)
    print(f"select_checks: twister={decision}", file=sys.stderr)
    if args.github_output and os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8", newline="\n") as fh:
            fh.write(f"twister={decision}\n")
    print(decision)
    return 0


if __name__ == "__main__":
    sys.exit(main())
