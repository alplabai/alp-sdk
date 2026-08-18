# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_public_private.py."""

from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


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


def test_detects_lab_infra_hostname(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "Ops host: `erp" ".alplab.ai` runs the VPS.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"LAB_INFRA_HOSTNAME"}


def test_lab_infra_hostname_ignores_known_public_subdomains(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "See `community.alplab.ai` and `docs.alplab.ai`, or the placeholder\n"
        "`broker.example.alplab.ai` used in example code.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_detects_lab_infra_ip(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        "docs/example.md",
        "VPS address: `31.97" ".73.18` is reachable directly.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"LAB_INFRA_IP"}


def test_lab_infra_ip_ignores_reserved_and_documentation_ranges(tmp_path: Path) -> None:
    # Reserved/private/loopback/link-local (RFC 1918) and documentation
    # (RFC 5737) ranges are legitimate in bench/driver text; example.com's
    # own long-documented public IP is a legitimate stable ping/TCP target,
    # not a lab endpoint.
    path = _write(
        tmp_path,
        "docs/example.md",
        "Loopback `127.0.0.1`, unspecified `0.0.0.0`, link-local `169.254.1.1`,\n"
        "private `10.1.2.3`, `172.20.0.5`, `192.168.1.1`, documentation\n"
        "`192.0.2.10`, `198.51.100.10`, `203.0.113.10`, and example.com's\n"
        "`93.184.216.34` are all fine here.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_lab_infra_ip_ignores_non_backticked_spec_numbers(tmp_path: Path) -> None:
    # Dotted-quad-shaped spec/clause and version citations are common in
    # this tree and are never backtick-quoted -- only a code-span IPv4
    # literal is a finding.
    path = _write(
        tmp_path,
        "docs/example.md",
        "IEEE 802.3 clause 22.2.4.1 and nanopb version 0.4.9.1" " (no backticks).\n",
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


def test_known_pending_is_empty(tmp_path: Path) -> None:
    # The CHANGELOG.md PCB_ROUTING_DETAIL exemption this tuple held was
    # resolved by maintainer ruling (redact) -- nothing should be exempted
    # from any rule any more.  A `PCB_ROUTING_DETAIL` trigger on any line of
    # any file is a real finding now.
    assert classifier.KNOWN_PENDING == ()
    path = _write(
        tmp_path,
        "CHANGELOG.md",
        "Routing note: 5 mm length" " matching applied here.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PCB_ROUTING_DETAIL"}


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


def test_superpowers_has_no_per_rule_exemption(tmp_path: Path) -> None:
    # Issue #1515 measured a per-rule docs/superpowers opt-out for
    # PRIVATE_AUDIT_REFERENCE against the live tree (98 files / 50,112
    # lines) and got zero hits: the false-positive risk the exemption was
    # meant to absorb never materialized, so the per-rule
    # ``scan_superpowers`` flag was deleted rather than kept unexercised
    # (see check_public_private.py's module docstring).  Every rule --
    # PRIVATE_AUDIT_REFERENCE included, plus the other design-secrecy rules
    # (PRIVATE_DESIGN_REFERENCE, SOM_PHYSICAL_DESIGN_DETAIL,
    # PCB_ROUTING_DETAIL) and a lab-endpoint rule (LAB_SSH_ENDPOINT) -- now
    # scans docs/superpowers exactly like any other tracked tree.
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
    design_note = _write(
        tmp_path,
        "docs/superpowers/plans/example3.md",
        "See the private" " repo netlist for the routing.\n",
    )
    som_note = _write(
        tmp_path,
        "docs/superpowers/plans/example4.md",
        "Flashed via bench" " rework fixture for this step.\n",
    )
    pcb_note = _write(
        tmp_path,
        "docs/superpowers/plans/example5.md",
        "Notes on the layer" " stackup for this rev.\n",
    )
    findings = classifier.scan(
        [audit_note, ssh_note, design_note, som_note, pcb_note], base=tmp_path
    )
    categories = {f.category for f in findings}
    assert categories == {
        "PRIVATE_AUDIT_REFERENCE",
        "LAB_SSH_ENDPOINT",
        "PRIVATE_DESIGN_REFERENCE",
        "SOM_PHYSICAL_DESIGN_DETAIL",
        "PCB_ROUTING_DETAIL",
    }


def test_rule_has_no_scan_superpowers_field(tmp_path: Path) -> None:
    # Guard against the flag silently coming back: Rule carries no
    # per-category docs/superpowers opt-out any more (see
    # test_superpowers_has_no_per_rule_exemption above for why).
    assert "scan_superpowers" not in classifier.Rule.__dataclass_fields__


def test_json_output(tmp_path: Path) -> None:
    _write(tmp_path, "README.md", "Use /home/" "caner/worktree.\n")
    proc = _run("--root", str(tmp_path), "--json")
    assert proc.returncode == 1
    payload = json.loads(proc.stdout)
    assert payload["category"] == "LOCAL_MAINTAINER_PATH"
    assert payload["path"] == "README.md"


def test_live_repo_is_clean() -> None:
    # Issue #524's six long-standing findings are now closed: two (the
    # CHANGELOG's audit-doc removal/relocation bullet) are exempt because
    # the PRIVATE_AUDIT_REFERENCE rule recognises the paragraph itself as
    # describing that removal (see
    # test_private_audit_reference_exempt_when_paragraph_describes_removal);
    # the remaining four (a plain carrier-errata pointer and three
    # historical audit-filename citations that predate the relocation) are
    # REVIEWED_ACCEPTED, each with its own maintainer-reviewed reason.  Any
    # NEW finding fails this gate loudly.
    proc = _run("--root", str(REPO))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_reviewed_accepted_requires_reason() -> None:
    # Requirement: a missing/empty reason must refuse to start, not exempt
    # silently.
    bad = (
        classifier.ReviewedAcceptedEntry(
            path="docs/example.md",
            line=1,
            category="LOCAL_MAINTAINER_PATH",
            excerpt="something",
            reason="",
        ),
    )
    with pytest.raises(SystemExit):
        classifier._validate_reviewed_accepted(bad)


def test_reviewed_accepted_requires_excerpt() -> None:
    # Requirement: a bare line-number anchor (no excerpt) must also refuse
    # to start -- a REVIEWED_ACCEPTED entry has to be re-derivable from real
    # file content, not just a number that can silently drift onto the
    # wrong line.
    bad = (
        classifier.ReviewedAcceptedEntry(
            path="docs/example.md",
            line=1,
            category="LOCAL_MAINTAINER_PATH",
            excerpt="",
            reason="a real reason",
        ),
    )
    with pytest.raises(SystemExit):
        classifier._validate_reviewed_accepted(bad)


def test_reviewed_accepted_survives_line_shift_from_prepended_content(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # REGRESSION TEST for the measured defect: REVIEWED_ACCEPTED matches on
    # (path, category, excerpt) -- NOT on `line` (see _is_reviewed_accepted).
    # `line` is a human-facing hint only now.  Content inserted ABOVE an
    # accepted finding shifts every later line number -- exactly what
    # prepending a CHANGELOG.md entry does in the real repo (one new
    # `## [Unreleased]` entry shifted all four REVIEWED_ACCEPTED entries off
    # their true lines and turned them back into findings) -- the exemption
    # must still apply after the shift.
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/example.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="/home/" "caner",
                reason="test fixture: the allowlisted line itself.",
            ),
        ),
    )
    exempted = _write(tmp_path, "docs/example.md", "Use /home/" "caner/ti/sdk here.\n")
    assert classifier.scan([exempted], base=tmp_path) == []

    # `line=1` in the entry above no longer matches where the text actually
    # lives once a filler line is prepended -- still exempt, because
    # matching is by content, not position.  (`_write` dedents and strips
    # *leading* blank lines, so a non-blank filler line pushes the trigger
    # to line 2 -- a bare "\n" prefix would be stripped and land back on
    # line 1.)
    shifted = _write(
        tmp_path,
        "docs/example.md",
        "Filler line inserted above, shifting the trigger to line 2.\n"
        "Use /home/" "caner/ti/sdk here.\n",
    )
    findings = classifier.scan([shifted], base=tmp_path)
    assert findings == []


def test_reviewed_accepted_ambiguous_excerpt_exempts_nothing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # Matching a REVIEWED_ACCEPTED excerpt is by content, not line position,
    # so an excerpt that isn't unique in its file must not silently exempt
    # every line it happens to match -- including a brand-new line nobody
    # reviewed.  scan()'s per-file `active_entries` filter (see its
    # docstring on ReviewedAcceptedEntry) refuses to suppress ANY line for
    # an entry whose excerpt matches more than one -- both lines below come
    # back as real findings, not zero.
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/example.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="/home/" "caner",
                reason="test fixture: deliberately broad (non-unique) excerpt.",
            ),
        ),
    )
    path = _write(
        tmp_path,
        "docs/example.md",
        "Use /home/" "caner/ti/sdk here.\n"
        "A second, unrelated mention of /home/" "caner/elsewhere too.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 2
    assert {f.line for f in findings} == {1, 2}
    assert {f.category for f in findings} == {"LOCAL_MAINTAINER_PATH"}


def test_reviewed_accepted_integrity_messages_reports_stale_excerpt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # An entry whose excerpt appears NOWHERE in the file it names (the
    # reviewed content was deleted, not just moved) must be reported, not
    # silently exempt nothing -- but as a returned message, never a raise:
    # _reviewed_accepted_integrity_messages is a separate, non-raising
    # function called from main() after scan() returns (see its own
    # docstring), so this diagnostic can never discard a run's real
    # findings; scan() itself is never involved in producing it.
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/example.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="this excerpt does not appear on the real line",
                reason="test fixture: deliberately stale excerpt.",
            ),
        ),
    )
    path = _write(tmp_path, "docs/example.md", "Use /home/" "caner/ti/sdk here.\n")
    messages = classifier._reviewed_accepted_integrity_messages(tmp_path, [path])
    assert len(messages) == 1
    assert "stale excerpt" in messages[0]
    assert "docs/example.md (LOCAL_MAINTAINER_PATH)" in messages[0]


def test_reviewed_accepted_integrity_messages_reports_ambiguous_excerpt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # The diagnostic counterpart to
    # test_reviewed_accepted_ambiguous_excerpt_exempts_nothing above: the
    # >1-match case is reported by name too, not just silently un-suppressed.
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/example.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="/home/" "caner",
                reason="test fixture: deliberately broad (non-unique) excerpt.",
            ),
        ),
    )
    path = _write(
        tmp_path,
        "docs/example.md",
        "Use /home/" "caner/ti/sdk here.\n"
        "A second, unrelated mention of /home/" "caner/elsewhere too.\n",
    )
    messages = classifier._reviewed_accepted_integrity_messages(tmp_path, [path])
    assert len(messages) == 1
    assert "ambiguous excerpt" in messages[0]
    assert "matches 2 lines" in messages[0]


def test_reviewed_accepted_integrity_messages_skips_paths_outside_this_runs_scan(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # An entry for a file outside the current run's selection is skipped by
    # the `e.path not in scanned_rels` filter itself -- not merely because
    # the file happens not to exist (that would go through `except OSError:
    # continue` instead and prove nothing about the filter).  So
    # docs/not-scanned.md is written for real, with content that WOULD
    # produce a stale-excerpt message if the filter were missing; only the
    # filter is what keeps it out.
    _write(
        tmp_path,
        "docs/not-scanned.md",
        "Real content on disk, but this run never selects this file.\n",
    )
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/not-scanned.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="this excerpt does not appear on the real line",
                reason="test fixture: file outside this run's --path selection.",
            ),
        ),
    )
    other = _write(tmp_path, "docs/example.md", "Unrelated content.\n")
    assert classifier._reviewed_accepted_integrity_messages(tmp_path, [other]) == []


def test_stale_excerpt_does_not_discard_real_findings(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    # End to end through main(): a run with BOTH a stale REVIEWED_ACCEPTED
    # entry AND a real finding in the same file must still print and fail
    # on the real finding -- the integrity diagnostic is additive, never a
    # substitute for scan()'s own findings.
    monkeypatch.setattr(classifier, "REPO", tmp_path.resolve())
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="CHANGELOG.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="this excerpt does not appear anywhere in the file",
                reason="test fixture: deliberately stale excerpt.",
            ),
        ),
    )
    _write(tmp_path, "CHANGELOG.md", "Use /home/" "caner/ti/sdk here.\n")
    rc = classifier.main(["--root", str(tmp_path), "--path", "CHANGELOG.md"])
    captured = capsys.readouterr()
    assert rc == 1
    assert "LOCAL_MAINTAINER_PATH" in captured.out
    assert "stale excerpt" in captured.err


def test_integrity_message_alone_fails_the_run_with_zero_findings(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    # A stale/ambiguous REVIEWED_ACCEPTED entry must fail the run on its
    # own, even when scan() finds no ordinary leak in the content -- rc=1
    # here has no findings to point to (main()'s `findings or
    # integrity_messages` gate, not `findings` alone).
    monkeypatch.setattr(classifier, "REPO", tmp_path.resolve())
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="CHANGELOG.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="this excerpt does not appear anywhere in the file",
                reason="test fixture: deliberately stale excerpt.",
            ),
        ),
    )
    _write(tmp_path, "CHANGELOG.md", "Nothing private here at all.\n")
    rc = classifier.main(["--root", str(tmp_path), "--path", "CHANGELOG.md"])
    captured = capsys.readouterr()
    assert rc == 1
    assert captured.out == ""
    assert "stale excerpt" in captured.err


def test_clean_tree_under_different_root_exits_zero(tmp_path: Path) -> None:
    # REVIEWED_ACCEPTED's excerpts are a snapshot of THIS repo's own
    # CHANGELOG.md and must never be asked of an unrelated tree's
    # CHANGELOG.md: the integrity check only runs when `--root` resolves to
    # this actual checkout (`root == REPO`), so a private-clean tree under
    # any other root exits 0 regardless of what REVIEWED_ACCEPTED contains.
    _write(
        tmp_path,
        "CHANGELOG.md",
        "# Changelog\n\nNothing private here at all.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_reviewed_accepted_fail_closed_when_line_no_longer_carries_excerpt(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    # Fail-closed property still holds: if the specific line's text changes
    # so the excerpt no longer matches THAT line, the finding on it comes
    # back -- even though the excerpt phrase (and so the entry) survives
    # elsewhere in the same file, so this is not the stale-excerpt case
    # above (which fails loudly instead of returning a finding).
    monkeypatch.setattr(
        classifier,
        "REVIEWED_ACCEPTED",
        (
            classifier.ReviewedAcceptedEntry(
                path="docs/example.md",
                line=1,
                category="LOCAL_MAINTAINER_PATH",
                excerpt="reference note: /home/" "caner/ti/sdk",
                reason="test fixture: fail-closed on a reworded line.",
            ),
        ),
    )
    path = _write(
        tmp_path,
        "docs/example.md",
        "Reworded, no longer matching the excerpt: /home/" "caner/elsewhere/path.\n"
        "Historical " "reference note: /home/" "caner/ti/sdk lives on in this unrelated paragraph.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].line == 1
    assert findings[0].category == "LOCAL_MAINTAINER_PATH"


def test_private_audit_reference_exempt_when_paragraph_describes_removal(
    tmp_path: Path,
) -> None:
    # The generalised half of the fix: PRIVATE_AUDIT_REFERENCE does not fire
    # inside a bullet that itself records the cited artifact leaving the
    # public tree -- CHANGELOG.md's actual shape for this (issue #524).
    # Uses the bare filename as its excerpt-shaped text, not one of the
    # real REVIEWED_ACCEPTED entries' now-lengthened sentence excerpts (see
    # ReviewedAcceptedEntry's uniqueness bound), so this fixture cannot
    # collide with the real tuple even though it is untouched here.
    path = _write(
        tmp_path,
        "CHANGELOG.md",
        "Preamble text.\n"
        "\n"
        "- **`docs/aen-feature-au" "dit-2026-05.md` removed from the public\n"
        "  SDK.**  Internal product-coverage roadmap.  Moved to\n"
        "  `alplabai/e1m-som-metadata` as `AEN-FEATURE-AU" "DIT-2026-05.md`.\n"
        "\n"
        "Trailer text.\n",
    )
    assert classifier.scan([path], base=tmp_path) == []


def test_private_audit_reference_not_exempt_without_removal_language(
    tmp_path: Path,
) -> None:
    # Narrowness proof for the generalisation: the identical filename
    # citation, one paragraph over, with no removal/relocation language in
    # its own block, still fails -- the rule understands the *distinction*,
    # it does not stop checking the category.
    path = _write(
        tmp_path,
        "CHANGELOG.md",
        "Preamble text.\n"
        "\n"
        "- **`docs/aen-feature-au" "dit-2026-05.md` is cited here.**\n"
        "  No relocation language anywhere in this paragraph.\n"
        "\n"
        "Trailer text.\n",
    )
    findings = classifier.scan([path], base=tmp_path)
    assert {f.category for f in findings} == {"PRIVATE_AUDIT_REFERENCE"}
