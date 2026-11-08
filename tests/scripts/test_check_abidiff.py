# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""check_abidiff.py: baseline pick, exit-bit policy, loud skip (#2840)."""
import importlib.util
import subprocess
from pathlib import Path

import pytest

_p = Path(__file__).resolve().parents[2] / "scripts" / "check_abidiff.py"
_s = importlib.util.spec_from_file_location("check_abidiff", _p)
mod = importlib.util.module_from_spec(_s)
_s.loader.exec_module(mod)

TAGS = ["v0.16.0", "v0.17.0-rc1", "v0.17.0-rc2", "v0.17.0", "v0.17.1", "v0.17.10", "v0.18.0-rc1"]


def test_baseline_prefers_highest_release():
    assert mod.pick_baseline("0.17", TAGS) == "v0.17.10"


def test_baseline_falls_back_to_latest_rc():
    assert mod.pick_baseline("0.17", ["v0.17.0-rc1", "v0.17.0-rc2"]) == "v0.17.0-rc2"


def test_baseline_none_for_new_minor():
    assert mod.pick_baseline("0.19", TAGS) is None


@pytest.mark.parametrize("rc", [4, 8, 12])
def test_change_fails_in_same_minor(rc):
    failed, why = mod.evaluate(rc, same_minor=True)
    assert failed and "patch release" in why


def test_change_reports_only_for_other_minor():
    assert mod.evaluate(12, same_minor=False)[0] is False


def test_clean_and_tool_error():
    assert mod.evaluate(0, True) == (False, "no ABI change")
    assert mod.evaluate(1, True)[0] is True
    assert mod.evaluate(3, False)[0] is True


def test_missing_abidiff_skips_loudly(monkeypatch, capsys):
    monkeypatch.setattr(mod.shutil, "which", lambda t: None)
    monkeypatch.setattr("sys.argv", ["x"])
    assert mod.main() == 0
    assert "NOTHING WAS CHECKED" in capsys.readouterr().err


def test_missing_abidiff_fails_when_required(monkeypatch):
    monkeypatch.setattr(mod.shutil, "which", lambda t: None)
    monkeypatch.setattr("sys.argv", ["x", "--require-tool"])
    assert mod.main() == 1


def test_main_wires_abidiff_exit_code(tmp_path, monkeypatch):
    (tmp_path / "metadata").mkdir()
    (tmp_path / "metadata" / "sdk_version.yaml").write_text("version: 0.17.2\n", encoding="utf-8")
    monkeypatch.setattr(mod.shutil, "which", lambda t: "/usr/bin/" + t)
    monkeypatch.setattr(mod, "export_tag", lambda *a: None)
    monkeypatch.setattr(mod, "build_lib", lambda src, b: b / "libalp_sdk.so")
    monkeypatch.setattr(mod, "soname_of", lambda lib: "libalp_sdk.so.0.17")

    def fake_run(cmd, **kw):
        if cmd[0] == "git":
            return subprocess.CompletedProcess(cmd, 0, stdout="\n".join(TAGS), stderr="")
        assert cmd[0] == "abidiff" and "--no-added-syms" in cmd
        return subprocess.CompletedProcess(cmd, fake_run.rc, stdout="diff", stderr="")

    monkeypatch.setattr(mod.subprocess, "run", fake_run)
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path)])
    fake_run.rc = 12
    assert mod.main() == 1
    fake_run.rc = 0
    assert mod.main() == 0


def test_soname_only_noise_detected():
    only = "\n".join([
        "ELF SONAME changed",
        "Functions changes summary: 0 Removed, 0 Changed, 0 Added function",
        "Variables changes summary: 0 Removed, 0 Changed, 0 Added variable",
        "SONAME changed from 'a' to 'b'",
    ])
    assert mod.soname_only(only)
    assert not mod.soname_only(only.replace("0 Removed, 0 Changed, 0 Added function",
                                            "1 Removed, 0 Changed, 0 Added function"))


def test_soname_only_not_fooled_by_filtered_change():
    rep = "\n".join([
        "Functions changes summary: 0 Removed, 1 Changed (9 filtered out), 0 Added functions",
        "Variables changes summary: 0 Removed, 0 Changed, 0 Added variable",
        "SONAME changed from 'a' to 'b'",
    ])
    assert not mod.soname_only(rep)


# Real abidiff 2.9.0 output; see fixtures/abidiff/README.md.
FIX = Path(__file__).parent / "fixtures" / "abidiff"
GOLDEN = FIX / "soname_only.txt"
BREAK = (FIX / "break.txt").read_text(encoding="utf-8")


def test_soname_only_golden_report():
    assert mod.soname_only(GOLDEN.read_text(encoding="utf-8"))


def _wire(tmp_path, monkeypatch, rc, stdout):
    (tmp_path / "metadata").mkdir(exist_ok=True)
    (tmp_path / "metadata" / "sdk_version.yaml").write_text("version: 0.17.2\n", encoding="utf-8")
    monkeypatch.setattr(mod.shutil, "which", lambda t: "/usr/bin/" + t)
    monkeypatch.setattr(mod, "export_tag", lambda *a: None)
    monkeypatch.setattr(mod, "build_lib", lambda src, b: b / "libalp_sdk.so")
    monkeypatch.setattr(mod, "soname_of", lambda lib: lib.parent.name)  # differ

    def fake_run(cmd, **kw):
        if cmd[0] == "git":
            return subprocess.CompletedProcess(cmd, 0, stdout="v0.17.0-rc2", stderr="")
        return subprocess.CompletedProcess(cmd, rc, stdout=stdout, stderr="")

    monkeypatch.setattr(mod.subprocess, "run", fake_run)
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path)])


def test_soname_suppression_applies_on_plain_abi_change(tmp_path, monkeypatch):
    _wire(tmp_path, monkeypatch, 4, GOLDEN.read_text(encoding="utf-8"))
    assert mod.main() == 0


def test_soname_suppression_never_masks_abidiff_error(tmp_path, monkeypatch):
    _wire(tmp_path, monkeypatch, 5, GOLDEN.read_text(encoding="utf-8"))
    assert mod.main() == 1


def test_skip_warns_in_ci(monkeypatch, capsys):
    monkeypatch.setattr(mod.shutil, "which", lambda t: None)
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    monkeypatch.setattr("sys.argv", ["x"])
    assert mod.main() == 0
    assert "::warning" in capsys.readouterr().out


def test_no_tag_at_all_fails_under_require_tool(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, 0, "")
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    monkeypatch.setattr(mod.subprocess, "run",
                        lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, stdout="", stderr=""))
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path), "--require-tool"])
    assert mod.main() == 1
    assert "::warning" in capsys.readouterr().out


def test_no_tag_at_all_skips_without_require_tool(tmp_path, monkeypatch, capsys):
    _wire(tmp_path, monkeypatch, 0, "")
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    monkeypatch.setattr(mod.subprocess, "run",
                        lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, stdout="", stderr=""))
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path)])
    assert mod.main() == 0
    assert "SKIP" in capsys.readouterr().out


def test_previous_minor_baseline_ignores_feed_tags_and_orders_numerically():
    tags = ["feed-v0.17-qemu", "v0.9.0", "v0.10.0-rc1", "v0.10.0-rc9", "v0.10.0-rc10", "v0.8.2"]
    assert mod.pick_previous_minor_baseline("0.11", tags) == "v0.10.0-rc10"
    assert mod.pick_previous_minor_baseline("0.10", tags) == "v0.9.0"
    assert mod.pick_previous_minor_baseline("0.8", ["v0.8.2"]) is None


def test_baseline_ignores_feed_tags():
    assert mod.pick_baseline("0.17", ["feed-v0.17-qemux86", "v0.17.0-rc1"]) == "v0.17.0-rc1"
    assert mod.pick_baseline("0.17", ["feed-v0.17-qemux86"]) is None


def test_new_minor_compares_previous_minor_report_only(tmp_path, monkeypatch, capsys):
    """dev just bumped to 0.19 with no v0.19.* tag: must still diff, never fail on ABI change."""
    _wire(tmp_path, monkeypatch, 12, "diff")
    (tmp_path / "metadata" / "sdk_version.yaml").write_text("version: 0.19.0\n", encoding="utf-8")
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)

    def fake_run(cmd, **kw):
        if cmd[0] == "git":
            return subprocess.CompletedProcess(cmd, 0, stdout="v0.17.0 v0.18.0-rc1 v0.18.0", stderr="")
        return subprocess.CompletedProcess(cmd, 12, stdout="diff", stderr="")

    monkeypatch.setattr(mod.subprocess, "run", fake_run)
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path), "--require-tool"])
    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "::warning" in out and "v0.18.0" in out and "report-only" in out


def test_baseline_tag_of_other_minor_is_report_only(tmp_path, monkeypatch):
    _wire(tmp_path, monkeypatch, 12, "diff")
    monkeypatch.setattr("sys.argv", ["x", "--root", str(tmp_path), "--baseline-tag", "v0.16.0"])
    assert mod.main() == 0


@pytest.mark.parametrize("rc", [1, 2])
def test_main_abidiff_tool_error_fails(tmp_path, monkeypatch, rc):
    _wire(tmp_path, monkeypatch, rc, "")
    assert mod.main() == 1


# --- waivers (#2851), driven by real abidiff output ---------------------

YAML_ENTRY = """- symbol: {symbol}
  kind: {kind}
  reason: "{reason}"
  issue: "{issue}"
  until: "{until}"
"""


def _entry(symbol, kind="function", reason="reviewed", issue="#2851", until="0.19"):
    return YAML_ENTRY.format(symbol=symbol, kind=kind, reason=reason, issue=issue, until=until)


ALL_WAIVERS = "".join([
    _entry("gone"), _entry("sig"), _entry("use_cfg"),
    _entry("g_level", "variable"), _entry("g_mode", "variable"),
])


def _waive(tmp_path, monkeypatch, yaml_text, rc=12, stdout=BREAK):
    _wire(tmp_path, monkeypatch, rc, stdout)
    d = tmp_path / "docs" / "abi"
    d.mkdir(parents=True, exist_ok=True)
    (d / "abidiff-waivers.yaml").write_text(yaml_text, encoding="utf-8")


def test_real_report_parses_into_entries():
    ents = mod.parse_entries(BREAK)
    assert [(e["kind"], e["name"]) for e in ents] == [
        ("function", "gone"), ("function", "sig"), ("function", "use_cfg"),
        ("variable", "g_level"), ("variable", "g_mode")]
    assert "struct cfg" in ents[2]["types"]


def test_unwaived_break_fails(tmp_path, monkeypatch):
    _waive(tmp_path, monkeypatch, "[]\n")
    assert mod.main() == 1


def test_partial_waiver_still_fails(tmp_path, monkeypatch):
    _waive(tmp_path, monkeypatch, _entry("gone") + _entry("sig"))
    assert mod.main() == 1


def test_waived_break_passes_and_reports(tmp_path, monkeypatch, capsys):
    _waive(tmp_path, monkeypatch, ALL_WAIVERS)
    assert mod.main() == 0
    out = capsys.readouterr().out
    assert "waived: function gone (#2851, until 0.19): reviewed" in out


def test_type_waiver_covers_changed_struct(tmp_path, monkeypatch):
    text = _entry("gone") + _entry("sig") + _entry("struct cfg", "type") + \
        _entry("g_level", "variable") + _entry("g_mode", "variable")
    _waive(tmp_path, monkeypatch, text)
    assert mod.main() == 0


def test_wrong_kind_does_not_waive(tmp_path, monkeypatch):
    _waive(tmp_path, monkeypatch, ALL_WAIVERS.replace("kind: variable", "kind: function"))
    assert mod.main() == 1


def test_expired_waiver_fails_even_without_diff(tmp_path, monkeypatch, capsys):
    _waive(tmp_path, monkeypatch, _entry("gone", until="0.17"), rc=0, stdout="")
    assert mod.main() == 1
    assert "expired" in capsys.readouterr().err


@pytest.mark.parametrize("bad", [
    _entry("gone", reason=""),
    _entry("gone", issue="2851"),
    _entry("gone", until="soon"),
    _entry("gone", kind="macro"),
    "- symbol: gone\n  kind: function\n  until: \"0.19\"\n",
    "not: a list\n",
    "- [broken\n",
])
def test_malformed_waiver_fails(tmp_path, monkeypatch, bad):
    _waive(tmp_path, monkeypatch, bad + "\n", rc=0, stdout="")
    assert mod.main() == 1


def test_waiver_never_masks_abidiff_error(tmp_path, monkeypatch):
    _waive(tmp_path, monkeypatch, ALL_WAIVERS, rc=13)
    assert mod.main() == 1


def test_soname_only_real_report_passes(tmp_path, monkeypatch):
    _wire(tmp_path, monkeypatch, 12, GOLDEN.read_text(encoding="utf-8"))
    assert mod.main() == 0


def test_error_bit_with_real_soname_text_fails(tmp_path, monkeypatch):
    for rc in (1, 2, 13):
        _wire(tmp_path, monkeypatch, rc, GOLDEN.read_text(encoding="utf-8"))
        assert mod.main() == 1


def test_shipped_waiver_file_is_valid():
    ok, bad = mod.load_waivers(mod.REPO / mod.WAIVER_FILE, mod.sdk_minor(mod.REPO))
    assert bad == []


# --- review follow-ups (#2851) ------------------------------------------

TYPE_PLUS_PARAM = (FIX / "type_plus_param.txt").read_text(encoding="utf-8")


def test_type_waiver_does_not_cover_function_with_other_changes(tmp_path, monkeypatch):
    """`take` takes the changed struct AND gained a parameter: waiving the struct is not enough."""
    _waive(tmp_path, monkeypatch, _entry("struct cfg", "type"), stdout=TYPE_PLUS_PARAM)
    assert mod.main() == 1
    covered, _ = mod.apply_waivers(TYPE_PLUS_PARAM, [dict(symbol="struct cfg", kind="type")])
    assert not covered


def test_function_waiver_covers_function_with_struct_and_param_change(tmp_path, monkeypatch):
    _waive(tmp_path, monkeypatch, _entry("take"), stdout=TYPE_PLUS_PARAM)
    assert mod.main() == 0


def test_unparsed_entry_fails_closed(tmp_path, monkeypatch):
    """Summary says 2 changes but only 1 parseable entry (e.g. a typedef/array variable)."""
    rep = BREAK.replace("1 Removed, 2 Changed", "1 Removed, 3 Changed")
    assert len(mod.parse_entries(rep)) == 5  # 6 expected from the summary
    _waive(tmp_path, monkeypatch, ALL_WAIVERS, stdout=rep)
    assert mod.main() == 1


def test_unquoted_until_and_issue_fail_through_yaml_loader(tmp_path, monkeypatch):
    unq_until = "- symbol: gone\n  kind: function\n  reason: r\n  issue: \"#1\"\n  until: 0.19\n"
    unq_issue = "- symbol: gone\n  kind: function\n  reason: r\n  issue: #12\n  until: \"0.19\"\n"
    for text in (unq_until, unq_issue):
        _waive(tmp_path, monkeypatch, text, rc=0, stdout="")
        assert mod.main() == 1


def test_waived_entries_with_unexplained_soname_change_still_fail(tmp_path, monkeypatch):
    rep = BREAK + "\nSONAME changed from 'libalp_sdk.so.0' to 'libalp_sdk.so.0.17'\n"
    _waive(tmp_path, monkeypatch, ALL_WAIVERS, stdout=rep)
    monkeypatch.setattr(mod, "soname_of", lambda lib: "libalp_sdk.so.0.17")  # sonames equal
    assert mod.main() == 1


def test_stale_waiver_warns_but_passes(tmp_path, monkeypatch, capsys):
    _waive(tmp_path, monkeypatch, ALL_WAIVERS + _entry("never_changed"))
    assert mod.main() == 0
    assert "warning: waiver function never_changed" in capsys.readouterr().out


def test_missing_waiver_file_needs_no_yaml(tmp_path, monkeypatch):
    monkeypatch.setitem(__import__("sys").modules, "yaml", None)  # import yaml -> ImportError
    assert mod.load_waivers(tmp_path / "absent.yaml", "0.18") == ([], [])
