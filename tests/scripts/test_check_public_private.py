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


# NOTE ON FIXTURE STRINGS BELOW: this test file is itself scanned by the
# gate it tests (scripts/ and tests/ are both default roots).  Fixtures
# that construct an offending string use adjacent string-literal
# concatenation split across the anchor words -- the runtime value is
# unchanged (Python concatenates with no glue characters), but no single
# *source* line then contains the trigger phrase, so the gate does not
# flag its own test suite.  `test_detects_maintainer_local_paths` below
# established this pattern; it is reused for every new rule.


def test_detects_maintainer_local_paths(tmp_path: Path) -> None:
    path = _write(tmp_path, "docs/bringup.md", "Use /home/" "caner/ti/sdk here.\n")
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "LOCAL_MAINTAINER_PATH"


def test_detects_private_audit_references(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "include/alp/gpu2d.h",
        "Surface rationale: internal AEN" " feature audit flagged this gap.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "PRIVATE_AUDIT_REFERENCE"


def test_detects_private_design_references(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "metadata/schemas/hw.json",
        "The schematic/netlist export lives"
        " privately under alp-sdk-internal.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PRIVATE_DESIGN_REFERENCE"}


def test_detects_som_physical_design_detail(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/aen-provisioning.md",
        "SEUART is on SoC" " balls A13/A14 for this SoM.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"SOM_PHYSICAL_DESIGN_DETAIL"}


def test_detects_dangling_private_notes_link(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "See memory/project" "_example_note.md for background.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"DANGLING_PRIVATE_NOTES_LINK"}


def test_dangling_private_notes_link_ignores_generic_memory_prose(tmp_path: Path) -> None:
    # "memory/MEMORY.md" and "peripherals/memory/USB" are real, legitimate
    # uses -- the rule keys off the project|feedback|reference naming
    # convention, not the bare word "memory".
    path = _write(
        tmp_path,
        "docs/example.md",
        "Ground the real peripherals/memory/USB address; see memory/MEMORY.md.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_detects_labgrid_place(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "Bench-proven on the labgrid" " place bench-rig-07 today.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"LABGRID_PLACE"}


def test_detects_probe_serial(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "Swapped probes: J-Link PLUS"
        " S/N"
        " 600107451 was the failing unit.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PROBE_SERIAL"}


def test_detects_lab_ssh_endpoint(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "Bench access: ssh root@10.0" ".0.77 to reach the unit.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"LAB_SSH_ENDPOINT"}


def test_lab_ssh_endpoint_ignores_placeholder_and_hostname(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "scp build/app root@<board-ip>:/usr/bin/\n"
        "ssh root@e1m-v2n101-a55.local /usr/bin/app\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_detects_pcb_routing_detail(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "CHANGELOG.md",
        "Routing note: 5 mm length" " matching applied to the clock pair.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PCB_ROUTING_DETAIL"}


def test_pcb_routing_detail_ignores_bare_numerics(tmp_path: Path) -> None:
    # A real shunt value or trace length is legitimate customer-facing
    # driver documentation -- only the layout-vocabulary phrases are
    # findings, never a bare ohm/mm number.
    path = _write(
        tmp_path,
        "include/alp/adc.h",
        "Populate a real 22 ohm shunt resistor on a 5 mm sense trace.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_known_pending_exempts_only_its_named_range(tmp_path: Path) -> None:
    pending = classifier.KNOWN_PENDING[0]
    assert pending.category == "PCB_ROUTING_DETAIL"
    trigger = "length" " matching noted here.\n"
    lines = ["filler\n"] * (pending.line_start - 1)
    lines.append(trigger)  # lands exactly at line_start -- exempted
    lines += ["filler\n"] * (pending.line_end - pending.line_start)
    lines.append(trigger)  # lands one line past line_end -- NOT exempted
    path = _write(tmp_path, pending.path, "".join(lines))
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].line == pending.line_end + 1


def test_normal_internal_language_is_not_flagged(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "include/alp/adc.h",
        "Use ALP_ADC_REF_INTERNAL for the on-chip internal reference.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_discover_files_skips_abi_but_scans_superpowers(tmp_path: Path) -> None:
    _write(tmp_path, "docs/abi/v0.8-snapshot.json", "/home/" "caner/old\n")
    _write(tmp_path, "docs/superpowers/plan.md", "customer-facing text\n")
    _write(tmp_path, "docs/live.md", "customer-facing text\n")
    found = {p.relative_to(tmp_path).as_posix() for p in classifier.discover_files(tmp_path)}
    assert "docs/live.md" in found
    assert "docs/abi/v0.8-snapshot.json" not in found
    assert "docs/superpowers/plan.md" in found


def test_superpowers_exempts_planning_vocabulary_but_not_lab_endpoints(tmp_path: Path) -> None:
    audit_note = _write(
        tmp_path,
        "docs/superpowers/plans/example.md",
        "Rationale: internal AEN" " feature audit informed this step.\n",
    )
    ssh_note = _write(
        tmp_path,
        "docs/superpowers/plans/example2.md",
        "Bench access: ssh root@10.0" ".0.88 for now.\n",
    )
    findings = classifier.scan([audit_note, ssh_note], base=tmp_path)
    categories = {f.category for f in findings}
    assert "PRIVATE_AUDIT_REFERENCE" not in categories
    assert categories == {"LAB_SSH_ENDPOINT"}


def test_json_output(tmp_path: Path) -> None:
    _write(tmp_path, "README.md", "Use /home/" "caner/worktree.\n")
    proc = _run("--root", str(tmp_path), "--json")
    assert proc.returncode == 1
    payload = json.loads(proc.stdout)
    assert payload["category"] == "LOCAL_MAINTAINER_PATH"
    assert payload["path"] == "README.md"


# Findings the live repo carries today beyond the CHANGELOG-PCB allowlist in
# scripts/check_public_private.py -- catalogued as maintainer-pending scrub /
# publication calls by issue #524 (see the implementor's report on that
# issue), deliberately NOT fixed here because Part 1 of that task scoped
# specific file:line targets and these fell outside every one of them.  This
# test pins the KNOWN set so the gate still fails loudly on anything NEW
# while these are triaged.  Update this set in the same commit that resolves
# (fixes, or adds a reviewed allowlist entry for) one of the items below.
KNOWN_LIVE_REPO_FINDINGS: frozenset[tuple[str, int, str]] = frozenset({
    ("CHANGELOG.md", 6870, "SOM_PHYSICAL_DESIGN_DETAIL"),
    ("CHANGELOG.md", 8128, "PRIVATE_AUDIT_REFERENCE"),
    ("CHANGELOG.md", 10191, "PRIVATE_AUDIT_REFERENCE"),
    ("CHANGELOG.md", 10195, "PRIVATE_AUDIT_REFERENCE"),
    ("CHANGELOG.md", 10957, "PRIVATE_AUDIT_REFERENCE"),
    ("CHANGELOG.md", 11119, "PRIVATE_AUDIT_REFERENCE"),
    ("docs/aen-bench-bringup.md", 459, "PROBE_SERIAL"),
    ("docs/superpowers/plans/2026-06-04-v2n-wifi-ble-port.md", 259, "LAB_SSH_ENDPOINT"),
    ("docs/superpowers/plans/2026-06-04-v2n-wifi-ble-port.md", 266, "LAB_SSH_ENDPOINT"),
    ("examples/peripheral-io/alp-console/src/main.c", 82, "DANGLING_PRIVATE_NOTES_LINK"),
})


def test_live_repo_findings_are_the_known_tracked_set() -> None:
    proc = _run("--root", str(REPO), "--json")
    got = {
        (obj["path"], obj["line"], obj["category"])
        for obj in (json.loads(line) for line in proc.stdout.splitlines() if line)
    }
    assert got == KNOWN_LIVE_REPO_FINDINGS, (
        "live-repo findings drifted from the tracked set -- fix the scrub, "
        "add a maintainer-reviewed allowlist entry, or update this test\n"
        + proc.stdout
    )
