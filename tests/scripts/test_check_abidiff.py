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


# Fixture in libabigail's documented abidiff report shape (SONAME-only change).
# ponytail: not recorded from a live run (no abidiff on the authoring host);
# replace with real rc2-vs-HEAD output when one is available.
GOLDEN = Path(__file__).parent / "fixtures" / "abidiff_soname_only.txt"


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
