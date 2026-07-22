# SPDX-License-Identifier: Apache-2.0
"""
Tests for `scripts/alp_project.py --emit scaffold --template <id> --sku
<SKU>` (issue #864): the {path, contents}[] envelope tan-cli vendors at
release instead of hand-porting a per-SKU Rust generator.

CLI-level (subprocess), matching tests/scripts/_project_support.py's
convention for scripts/alp_project.py -- the unit-level SKU-substitution
coverage lives in test_alp_template.py against `render_to_envelope()`
directly.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ALP_PROJECT = REPO / "scripts" / "alp_project.py"
HELLO_WORLD = REPO / "examples" / "peripheral-io" / "hello-world"


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(ALP_PROJECT), *args],
        capture_output=True, text=True, cwd=REPO, check=False,
    )


def test_scaffold_emits_json_envelope_for_the_examples_own_sku():
    proc = _run("--emit", "scaffold", "--template", "minimal",
                "--sku", "E1M-AEN801")
    assert proc.returncode == 0, proc.stderr
    envelope = json.loads(proc.stdout)
    # testcase.yaml is not part of the envelope (dropped from
    # metadata/templates/catalog-v1.json `files.user_owned` -- issue
    # #864 follow-up: SDK CI wiring, not a user's project file).
    assert {item["path"] for item in envelope} == {
        "board.yaml", "prj.conf", "CMakeLists.txt", "src/main.c",
        "README.md",
    }
    by_path = {item["path"]: item["contents"] for item in envelope}
    # board.yaml/prj.conf/src/main.c -- byte-identical passthrough for
    # the template's own SKU. CMakeLists.txt/README.md are scaffold-
    # adapted regardless of SKU (ALP_SDK_ROOT hardening / dangling-link
    # rewrite), so they differ even for the canonical example's own SKU.
    for rel in ("board.yaml", "prj.conf", "src/main.c"):
        assert by_path[rel] == (HELLO_WORLD / rel).read_text(encoding="utf-8"), rel
    assert "--core m55_hp" in by_path["CMakeLists.txt"]
    assert "ALP_SDK_ROOT is not set" in by_path["CMakeLists.txt"]


def test_scaffold_substitutes_sku_and_preset_for_a_different_sku():
    proc = _run("--emit", "scaffold", "--template", "minimal",
                "--sku", "E1M-V2N101")
    assert proc.returncode == 0, proc.stderr
    envelope = {item["path"]: item["contents"] for item in json.loads(proc.stdout)}
    assert "sku: E1M-V2N101" in envelope["board.yaml"]
    assert "preset: e1m-x-evk" in envelope["board.yaml"]
    # The AEN-only app core (m55_hp) is re-derived to V2N101's own
    # Zephyr core -- the #864 follow-up blocker fix: VERIFIED
    # `alp_project.py --input <old board.yaml> --emit zephyr-conf
    # --core m55_hp` against an E1M-V2N101 board.yaml used to fail
    # with rc=1, "unknown core id".
    assert "m33_sm:" in envelope["board.yaml"]
    assert "m55_hp" not in envelope["board.yaml"]
    assert "--core m33_sm" in envelope["CMakeLists.txt"]
    # prj.conf / src/main.c carry no sku-specific content -- unmodified.
    for rel in ("prj.conf", "src/main.c"):
        assert envelope[rel] == (HELLO_WORLD / rel).read_text(encoding="utf-8")

    # The derived core must actually be valid for E1M-V2N101 -- this is
    # the regression coverage for the blocker itself.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        board_yaml_path = Path(tmp) / "board.yaml"
        board_yaml_path.write_text(envelope["board.yaml"], encoding="utf-8")
        check = subprocess.run(
            [sys.executable, str(ALP_PROJECT), "--input", str(board_yaml_path),
             "--emit", "zephyr-conf", "--core", "m33_sm"],
            capture_output=True, text=True, cwd=REPO, check=False)
        assert check.returncode == 0, check.stderr


def test_scaffold_rejects_unsupported_sku_with_the_supported_list():
    proc = _run("--emit", "scaffold", "--template", "minimal", "--sku", "FOO")
    assert proc.returncode != 0
    assert "FOO" in proc.stderr
    assert "E1M-AEN801" in proc.stderr
    assert "E1M-V2N101" in proc.stderr


def test_scaffold_requires_template():
    proc = _run("--emit", "scaffold", "--sku", "E1M-AEN801")
    assert proc.returncode != 0
    assert "--template" in proc.stderr


def test_scaffold_requires_sku():
    proc = _run("--emit", "scaffold", "--template", "minimal")
    assert proc.returncode != 0
    assert "--sku" in proc.stderr


def test_scaffold_is_deterministic_across_two_invocations():
    a = _run("--emit", "scaffold", "--template", "minimal", "--sku", "E1M-V2N101")
    b = _run("--emit", "scaffold", "--template", "minimal", "--sku", "E1M-V2N101")
    assert a.returncode == 0 and b.returncode == 0
    assert a.stdout == b.stdout
