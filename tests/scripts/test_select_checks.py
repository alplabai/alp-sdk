# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/select_checks.py (change-aware twister selection).

A wrong ``skip`` silently drops coverage, so most of these pin the fail-safe
direction: every doubt must come out ``full``.  The tmp-tree tests pin each
rule; the real-tree tests re-prove the rule's premise on the checkout (a
build file that starts reading docs/, or a CMake-time script outside the
computed closure, fails here rather than silently skipping twister).
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import select_checks as sc  # noqa: E402

AEN_ONLY = """\
tests:
  alp_sdk.examples.aen.demo:
    platform_allow:
      - alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he
    build_only: true
"""
NATIVE = """\
tests:
  alp_sdk.examples.app:
    platform_allow: native_sim/native/64
"""


def _tree(tmp_path: Path, files: dict[str, str]) -> Path:
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text), encoding="utf-8")
    return tmp_path


def _base_tree(tmp_path: Path, **extra: str) -> Path:
    files = {
        "zephyr/CMakeLists.txt": "zephyr_library()\n",
        "examples/x/app/CMakeLists.txt":
            "execute_process(COMMAND python3 ${ALP_SDK_ROOT}/scripts/alp_project.py)\n",
        "examples/x/app/testcase.yaml": NATIVE,
        "scripts/alp_project.py": "import alp_helper\n",
        "scripts/alp_helper.py": "X = 1\n",
        "scripts/check_something.py": "print('gate')\n",
    }
    files.update({k.replace("__", "/"): v for k, v in extra.items()})
    return _tree(tmp_path, files)


# -- rule 1: prose / CI / other-OS prefixes ---------------------------------

def test_docs_changelog_and_yocto_only_skip(tmp_path):
    root = _base_tree(tmp_path)
    decision, reasons = sc.classify(
        ["changelog.d/2440.md", "docs/testing.md", "meta-alp-sdk/recipes-x/x.bbappend",
         "tests/scripts/test_x.py", ".github/workflows/pr-doc-drift.yml",
         "examples/x/app/README.md"], root)
    assert decision == "skip", reasons
    assert len(reasons) == 6


def test_empty_change_set_is_full(tmp_path):
    assert sc.classify([], _base_tree(tmp_path))[0] == "full"


@pytest.mark.parametrize("path", [
    "include/alp/gpio.h", "src/common/x.c", "zephyr/Kconfig", "CMakeLists.txt",
    "metadata/chips/bme280.yaml", "west.yml", "brand-new-dir/file.txt",
    "tests/zephyr/foo/src/main.c", "examples/x/app/src/main.c", "pyproject.toml",
])
def test_build_inputs_and_unknown_paths_are_full(tmp_path, path):
    decision, reasons = sc.classify(["docs/a.md", path], _base_tree(tmp_path))
    assert decision == "full"
    assert reasons[0].startswith(path)


@pytest.mark.parametrize("path", sorted(sc.FULL_EXACT))
def test_files_that_define_how_twister_runs_are_full(tmp_path, path):
    assert sc.classify([path], _base_tree(tmp_path))[0] == "full"


# -- rule 2: scripts/ build-time closure ------------------------------------

def test_script_named_by_cmake_is_full(tmp_path):
    root = _base_tree(tmp_path)
    decision, reasons = sc.classify(["scripts/alp_project.py"], root)
    assert decision == "full"
    assert "closure" in reasons[0]


def test_module_imported_by_a_build_script_is_full(tmp_path):
    assert sc.classify(["scripts/alp_helper.py"], _base_tree(tmp_path))[0] == "full"


def test_unreferenced_gate_script_skips(tmp_path):
    assert sc.classify(["scripts/check_something.py"], _base_tree(tmp_path))[0] == "skip"


def test_data_file_inside_a_closure_package_is_full(tmp_path):
    root = _tree(_base_tree(tmp_path), {"scripts/alp_helper.py": "import emitpkg\n",
                                        "scripts/emitpkg/__init__.py": "",
                                        "scripts/emitpkg/render.py": "",
                                        "scripts/emitpkg/tmpl.j2": "{{ x }}\n"})
    assert "scripts/emitpkg/tmpl.j2" in sc.Tree(root).closure()
    assert sc.classify(["scripts/emitpkg/render.py"], root)[0] == "full"


def test_non_python_scripts_file_outside_bench_is_full(tmp_path):
    root = _base_tree(tmp_path, **{"scripts__requirements.txt": "west\n",
                                    "scripts__bench__aen__flash.sh": "echo\n"})
    assert sc.classify(["scripts/requirements.txt"], root)[0] == "full"
    assert sc.classify(["scripts/bench/aen/flash.sh"], root)[0] == "skip"


def test_a_comment_mention_does_not_enter_the_closure(tmp_path):
    root = _base_tree(tmp_path, zephyr__Kconfig="# see scripts/check_something.py\n")
    assert sc.classify(["scripts/check_something.py"], root)[0] == "skip"


def test_a_non_comment_mention_enters_the_closure(tmp_path):
    root = _base_tree(
        tmp_path, **{"zephyr__app.cmake": "set(X ${ROOT}/scripts/check_something.py) # used\n"})
    assert sc.classify(["scripts/check_something.py"], root)[0] == "full"


def test_a_directive_in_an_overlay_is_not_mistaken_for_a_comment(tmp_path):
    root = _base_tree(
        tmp_path, **{"examples__x__app__boards__b.overlay":
                     '#include "../../../../scripts/check_something.py"\n'})
    assert sc.classify(["scripts/check_something.py"], root)[0] == "full"


# -- rule 3: suites pinned off native_sim -----------------------------------

def test_aen_only_suite_skips(tmp_path):
    root = _base_tree(tmp_path, **{"examples__aen__demo__testcase.yaml": AEN_ONLY,
                                    "examples__aen__demo__src__main.c": "int main;\n"})
    decision, reasons = sc.classify(["examples/aen/demo/src/main.c"], root)
    assert decision == "skip", reasons
    assert "platform_allow" in reasons[0]


def test_the_suite_yaml_itself_is_full_even_when_pinned_off_native_sim(tmp_path):
    # twister parses and schema-checks every testcase.yaml it discovers, so a
    # broken AEN-only one still fails the native_sim run.
    root = _base_tree(tmp_path, **{"examples__aen__demo__testcase.yaml": AEN_ONLY})
    assert sc.classify(["examples/aen/demo/testcase.yaml"], root)[0] == "full"


def test_suite_that_allows_native_sim_is_full(tmp_path):
    root = _base_tree(tmp_path)
    (root / "examples/x/app/src").mkdir()
    assert sc.classify(["examples/x/app/src/main.c"], root)[0] == "full"


def test_suite_without_platform_allow_is_full(tmp_path):
    root = _base_tree(tmp_path, **{
        "examples__aen__demo__testcase.yaml": "tests:\n  a.b:\n    tags: [x]\n"})
    assert sc.classify(["examples/aen/demo/prj.conf"], root)[0] == "full"


def test_one_native_scenario_among_many_is_full(tmp_path):
    root = _base_tree(tmp_path, **{
        "examples__aen__demo__testcase.yaml": AEN_ONLY + "  alp_sdk.examples.aen.sim:\n"
                                              "    platform_allow: native_sim\n"})
    assert sc.classify(["examples/aen/demo/prj.conf"], root)[0] == "full"


def test_suite_referenced_from_elsewhere_is_full(tmp_path):
    root = _base_tree(tmp_path, **{
        "examples__aen__demo__testcase.yaml": AEN_ONLY,
        "examples__x__app__src__main.c": '#include "../../../aen/demo/src/shared.h"\n'})
    assert sc.classify(["examples/aen/demo/src/shared.h"], root)[0] == "full"


def test_suite_named_only_in_a_comment_or_description_still_skips(tmp_path):
    root = _base_tree(tmp_path, **{
        "examples__aen__demo__testcase.yaml": AEN_ONLY,
        "examples__x__app__src__main.c": "/* same idea as aen demo */\n// see demo\n",
        "examples__x__app__testcase.yaml": NATIVE.replace(
            "tests:", "sample:\n  description: like demo\ntests:")})
    assert sc.classify(["examples/aen/demo/prj.conf"], root)[0] == "skip"


def test_nested_suite_is_full(tmp_path):
    root = _base_tree(tmp_path, **{
        "examples__aen__demo__testcase.yaml": AEN_ONLY,
        "examples__aen__demo__peer__testcase.yaml": NATIVE})
    assert sc.classify(["examples/aen/demo/src/shared.c"], root)[0] == "full"


# -- git plumbing + CLI fail-safes ------------------------------------------

def _git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True,
                   env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                        "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"})


def test_cli_diffs_against_the_merge_base_and_sees_untracked_files(tmp_path, capsys):
    root = _base_tree(tmp_path)
    _git(root, "init", "-q", "-b", "dev")
    _git(root, "add", "-A")
    _git(root, "commit", "-qm", "base")
    _git(root, "checkout", "-qb", "topic")
    (root / "docs").mkdir()
    (root / "docs" / "a.md").write_text("x\n", encoding="utf-8")
    _git(root, "add", "-A")
    _git(root, "commit", "-qm", "docs")
    assert sc.main(["--base", "dev", "--root", str(root)]) == 0
    assert capsys.readouterr().out.strip() == "skip"
    (root / "src").mkdir()
    (root / "src" / "new.c").write_text("int x;\n", encoding="utf-8")
    sc.main(["--base", "dev", "--root", str(root)])
    assert capsys.readouterr().out.strip() == "skip"      # untracked: not asked for
    sc.main(["--base", "dev", "--worktree", "--root", str(root)])
    assert capsys.readouterr().out.strip() == "full"      # untracked: counted


def test_cli_unresolvable_base_is_full(tmp_path, capsys):
    root = _base_tree(tmp_path)
    _git(root, "init", "-q")
    assert sc.main(["--base", "no-such-ref", "--root", str(root)]) == 0
    captured = capsys.readouterr()
    assert captured.out.strip() == "full"
    assert "fail-safe" in captured.err


def test_cli_without_base_is_full(capsys):
    sc.main([])
    assert capsys.readouterr().out.strip() == "full"


def test_cli_writes_github_output(tmp_path, monkeypatch, capsys):
    out = tmp_path / "gh_out"
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    sc.main(["--files", "docs/a.md", "--root", str(_base_tree(tmp_path)), "--github-output"])
    assert out.read_text(encoding="utf-8") == "twister=skip\n"


# -- the real tree: re-prove each rule's premise ----------------------------

@pytest.fixture(scope="module")
def real_tree():
    return sc.Tree(REPO)


def test_real_closure_is_not_vacuous(real_tree):
    closure = real_tree.closure()
    assert "scripts/alp_project.py" in closure
    assert any(p.startswith("scripts/alp_orchestrate/") for p in closure)


_READ_DIRECTIVE = re.compile(
    r"#\s*include\s*[<\"]([^>\"]+)"                                # C / DTS
    r"|\b(?:include|add_subdirectory|configure_file|file)\s*\(([^)]*)\)"  # CMake
    r"|^\s*[or]?source\s+\"?([^\s\"]+)",                           # Kconfig
    re.MULTILINE)


def test_no_build_file_reads_a_skipped_prefix_or_markdown(real_tree):
    """Rule 1's premise: nothing a native_sim build compiles, includes or
    sources points into docs/, changelog.d/, meta-alp-sdk/, tests/scripts/ ...
    or at a .md file."""
    prefixes = tuple(p for p in sc.SKIP_PREFIXES if p.endswith("/"))
    bad = []
    for rel, text in real_tree.sources().items():
        for m in _READ_DIRECTIVE.finditer(text):
            arg = next(g for g in m.groups() if g is not None)
            if re.search(r"\.md\b", arg) or any(p in arg for p in prefixes):
                bad.append(f"{rel}: {m.group(0).strip()}")
    assert not bad, bad


@pytest.mark.parametrize("argv", [
    ["--input", "examples/connectivity/iot-dashboard/board.yaml",
     "--emit", "zephyr-conf", "--core", "m55_hp"],
    ["--input", "examples/multicore/heterogeneous-offload/board.yaml",
     "--emit", "ipc-contract-h"],
])
def test_cmake_time_alp_project_reads_nothing_selection_skips(tmp_path, argv, real_tree):
    """Rules 1+2, measured: run the CMake-time command under an audit hook.
    Every repo file it opens must be something selection treats as a build
    input -- a scripts/ file in the closure, never a skipped prefix/.md."""
    if not (REPO / argv[1]).is_file():
        pytest.skip(f"{argv[1]} moved; update this representative invocation")
    log = tmp_path / "opened.txt"
    probe = textwrap.dedent(f"""
        import os, runpy, sys
        opened = set()
        def hook(event, args):
            if event == "open" and isinstance(args[0], (str, os.PathLike)):
                opened.add(os.path.abspath(os.fspath(args[0])))
        sys.addaudithook(hook)
        sys.path.insert(0, {str(REPO / 'scripts')!r})
        sys.argv = ["alp_project.py"] + {argv!r} + ["--output", {str(tmp_path / 'out')!r}]
        try:
            runpy.run_path({str(REPO / 'scripts' / 'alp_project.py')!r}, run_name="__main__")
        finally:
            open({str(log)!r}, "w", encoding="utf-8").write("\\n".join(sorted(opened)))
        """)
    subprocess.run([sys.executable, "-c", probe], cwd=REPO, check=True, capture_output=True)
    root = str(REPO) + os.sep
    rels = [p[len(root):].replace(os.sep, "/") for p in log.read_text(encoding="utf-8").splitlines()
            if p.startswith(root)]
    assert rels, "audit hook saw no repo file -- the probe is broken"
    closure = real_tree.closure()
    for rel in rels:
        if "__pycache__" in rel:
            continue
        if rel.startswith("scripts/"):
            assert rel in closure, f"{rel} is read at CMake time but outside the closure"
        else:
            assert sc.classify([rel], REPO, real_tree)[0] == "full", f"{rel} read at CMake time but skippable"
