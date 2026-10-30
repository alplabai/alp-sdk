# SPDX-License-Identifier: Apache-2.0
"""Workflow gates must be reached, point at real inputs, and not splice secrets.

Pins four review findings against `.github/workflows/`:

* pr-metadata-validate.yml hosts `check_vendor_ext_tags.py` and
  `check_public_header_purity.py`; its `paths:` filter must cover
  `include/alp/ext/**` and the root `include/alp/*.h`, or a header-only PR never
  runs either gate.
* pr-alp-build.yml matrix cells must name examples that exist, and every cell
  must carry its own `som` so each leg builds its own SoM.
* coverity.yml must not splice `secrets.*` into `run:` shell source.
* pr-static-analysis.yml cppcheck must be able to fail the job.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

_REPO_ROOT = Path(__file__).resolve().parents[2]
_WF = _REPO_ROOT / ".github" / "workflows"


def _load(name: str) -> dict:
    return yaml.safe_load((_WF / name).read_text(encoding="utf-8"))


def _paths(name: str, trigger: str) -> list[str]:
    data = _load(name)
    triggers = data.get("on", data.get(True))
    return list(triggers[trigger]["paths"])


def _pattern_to_regex(pattern: str) -> re.Pattern[str]:
    """GitHub Actions path glob -> regex. `**` crosses `/`; `*` does not."""
    out = ""
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif pattern[i] == "*":
            out += "[^/]*"
            i += 1
        else:
            out += re.escape(pattern[i])
            i += 1
    return re.compile(rf"^{out}$")


def _covered(probe: str, patterns: list[str]) -> bool:
    return any(
        _pattern_to_regex(p).match(probe) for p in patterns if not p.startswith("!")
    )


def test_metadata_validate_triggers_on_public_headers() -> None:
    probes = [
        "include/alp/ext/renesas/drpai.h",
        "include/alp/gpio.h",
        "scripts/check_vendor_ext_tags.py",
        "scripts/check_public_header_purity.py",
    ]
    for trigger in ("pull_request", "push"):
        patterns = _paths("pr-metadata-validate.yml", trigger)
        for probe in probes:
            assert _covered(probe, patterns), f"{trigger} paths do not cover {probe}"


def test_alp_build_matrix_cells_use_existing_examples_and_distinct_soms() -> None:
    job = _load("pr-alp-build.yml")["jobs"]["alp-build"]
    cells = job["strategy"]["matrix"]["include"]
    assert cells, "matrix has no cells"
    seen = set()
    for cell in cells:
        board = _REPO_ROOT / cell["example"] / "board.yaml"
        assert board.is_file(), f"{cell} points at a missing example"
        assert re.fullmatch(r"E1M-[A-Z0-9]+", cell["som"])
        key = (cell["som"], cell["scenario"])
        assert key not in seen, f"duplicate matrix cell {key}"
        seen.add(key)
    # The SoM must actually reach the orchestrator input.
    pick = next(s for s in job["steps"] if s.get("id") == "pick")
    assert pick["env"]["SOM"] == "${{ matrix.som }}"
    assert "E1M-AEN701" in {c["som"] for c in cells}


def test_alp_build_has_no_silent_skip_gate() -> None:
    text = (_WF / "pr-alp-build.yml").read_text(encoding="utf-8")
    assert "present=false" not in text
    assert "steps.gate.outputs.present" not in text


def test_coverity_does_not_splice_secrets_into_shell() -> None:
    jobs = _load("coverity.yml")["jobs"]
    assert jobs
    for name, job in jobs.items():
        for step in job["steps"]:
            run = step.get("run")
            if run:
                assert "${{ secrets." not in run, (
                    f"{name}: step {step.get('name')!r} splices a secret into run:"
                )


def _cppcheck_gate_tail() -> str:
    job = _load("pr-static-analysis.yml")["jobs"]["cppcheck"]
    run = next(s["run"] for s in job["steps"] if s.get("name") == "Run cppcheck")
    marker = "|| true\n"
    assert marker in run, "cppcheck invocation no longer ends in '|| true'"
    return run.split(marker, 1)[1]


def test_cppcheck_step_fails_on_error_severity(tmp_path: Path) -> None:
    bash = shutil.which("bash")
    if bash is None:
        pytest.skip("bash not available")
    tail = _cppcheck_gate_tail()
    head = '<?xml version="1.0"?><results><errors>'
    bad = head + '<error id="nullPointer" severity="error" msg="x"/></errors></results>'
    ok = (
        head
        + '<error id="unusedVariable" severity="style" msg="x"/></errors></results>'
    )

    def run(xml: str) -> int:
        (tmp_path / "cppcheck.xml").write_text(xml, encoding="utf-8")
        return subprocess.run(
            [bash, "-c", tail],
            cwd=tmp_path,
            capture_output=True,
            check=False,
        ).returncode

    assert run(bad) != 0
    assert run(ok) == 0


def test_cppcheck_baseline_suppressions_are_scoped() -> None:
    job = _load("pr-static-analysis.yml")["jobs"]["cppcheck"]
    run = next(s["run"] for s in job["steps"] if s.get("name") == "Run cppcheck")
    for needed in (
        "syntaxError",
        "unknownMacro",
        "ctuOneDefinitionRuleViolation",
        "comparePointers",
    ):
        assert f"--suppress={needed}" in run, needed


def test_test_coverage_audit_uses_fail_on_gaps() -> None:
    jobs = _load("pr-metadata-validate.yml")["jobs"]
    hits = []
    for job in jobs.values():
        for step in job["steps"]:
            r = step.get("run", "")
            if "check_test_coverage.py" in r:
                hits.append(r)
    assert hits and all("--fail-on-gaps" in r for r in hits)
