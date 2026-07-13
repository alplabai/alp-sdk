# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_public_private.py."""

from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_public_private.py"

sys.path.insert(0, str(REPO / "scripts"))
import check_public_private as classifier  # noqa: E402


def _write(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        check=False,
    )


def test_detects_maintainer_local_paths(tmp_path: Path) -> None:
    path = _write(tmp_path, "docs/bringup.md", "Use /home/" "caner/ti/sdk here.\n")
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "LOCAL_MAINTAINER_PATH"


def test_detects_private_audit_references(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "include/alp/gpu2d.h",
        "Surface rationale: internal AEN feature audit flagged this gap.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "PRIVATE_AUDIT_REFERENCE"


def test_detects_private_design_references(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "metadata/schemas/hw.json",
        "The schematic/netlist export lives privately under alp-sdk-internal.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PRIVATE_DESIGN_REFERENCE"}


def test_detects_som_physical_design_detail(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/aen-provisioning.md",
        "SEUART is on SoC balls A13/A14 and SoM test points TP4/TP5.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"SOM_PHYSICAL_DESIGN_DETAIL"}


def test_normal_internal_language_is_not_flagged(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "include/alp/adc.h",
        "Use ALP_ADC_REF_INTERNAL for the on-chip internal reference.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_discover_files_skips_abi_and_superpower_docs(tmp_path: Path) -> None:
    _write(tmp_path, "docs/abi/v0.8-snapshot.json", "/home/" "caner/old\n")
    _write(tmp_path, "docs/superpowers/plan.md", "internal AEN feature audit\n")
    _write(tmp_path, "docs/live.md", "customer-facing text\n")
    found = {p.relative_to(tmp_path).as_posix() for p in classifier.discover_files(tmp_path)}
    assert "docs/live.md" in found
    assert "docs/abi/v0.8-snapshot.json" not in found
    assert "docs/superpowers/plan.md" not in found


def test_json_output(tmp_path: Path) -> None:
    _write(tmp_path, "README.md", "Use /home/" "caner/worktree.\n")
    proc = _run("--root", str(tmp_path), "--json")
    assert proc.returncode == 1
    payload = json.loads(proc.stdout)
    assert payload["category"] == "LOCAL_MAINTAINER_PATH"
    assert payload["path"] == "README.md"


def test_live_repo_is_clean() -> None:
    proc = _run("--root", str(REPO))
    assert proc.returncode == 0, proc.stdout + proc.stderr
