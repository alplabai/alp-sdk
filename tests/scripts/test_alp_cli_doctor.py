"""Tests for `alp doctor` -- the read-only environment preflight.

Everything here monkeypatches the probing surface (shutil.which, the env,
sys.version_info, the `_tool_version` shell-out helper) so the tests are
hermetic: they never actually inspect this host's toolchain.
"""

from __future__ import annotations

import json
from collections import namedtuple

from click.testing import CliRunner

from alp_cli import doctor
from alp_cli.main import cli

# Mirror the attribute surface of sys.version_info (major/minor/micro/...).
_VInfo = namedtuple("_VInfo", "major minor micro releaselevel serial")


def _run(args=None):
    return CliRunner().invoke(cli, ["doctor", *(args or [])])


def _force_all_pass(monkeypatch, tmp_path):
    """Stub every probe so an otherwise-bare host reports all PASS."""
    # Tools all present.
    monkeypatch.setattr(doctor.shutil, "which", lambda name: f"/usr/bin/{name}")

    versions = {
        "west": (1, 2),
        "cmake": (3, 27),
        "gcc": (12, 2),
        "clang": (16, 0),
    }

    def _ver(args):
        return versions.get(args[0])

    monkeypatch.setattr(doctor, "_tool_version", _ver)

    # All Python deps importable (they really are in the test venv) -> leave
    # _check_python_deps alone.

    # A real Zephyr workspace on disk.
    ws = tmp_path / "zephyrproject"
    zephyr = ws / "zephyr"
    zephyr.mkdir(parents=True)
    (zephyr / "VERSION").write_text(
        "VERSION_MAJOR = 4\nVERSION_MINOR = 4\nVERSION_PATCH = 0\n", encoding="utf-8"
    )
    (ws / ".west").mkdir()
    (ws / ".venv").mkdir()
    monkeypatch.setenv("ZEPHYR_BASE", str(zephyr))

    # Make the workspace venv "active".
    monkeypatch.setattr(doctor.sys, "prefix", str(ws / ".venv"))
    # Point the SDK probe at a discoverable SDK.
    (tmp_path / doctor.ZEPHYR_SDK_VERSION).mkdir()
    monkeypatch.setenv("ZEPHYR_SDK_INSTALL_DIR", str(tmp_path / doctor.ZEPHYR_SDK_VERSION))
    # hal_alif present + USE_ALIF_HAL_* defined.
    (ws / "modules" / "hal" / "alif").mkdir(parents=True)
    monkeypatch.setattr(doctor, "_workspace_dir", lambda: ws)
    cfg = tmp_path / "alp.cmake"
    cfg.write_text("set(USE_ALIF_HAL_GPIO 1)\n", encoding="utf-8")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    # Non-Windows so the platform checks stay out.
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    return ws


# -------- individual check logic ---------------------------------------------


def test_python_below_310_fails(monkeypatch):
    monkeypatch.setattr(
        doctor.sys, "version_info", _VInfo(3, 9, 0, "final", 0)
    )
    res = doctor._check_python()
    assert res.status == doctor.FAIL
    assert "3.9" in res.message


def test_python_ok(monkeypatch):
    monkeypatch.setattr(doctor.sys, "version_info", _VInfo(3, 12, 1, "final", 0))
    assert doctor._check_python().status == doctor.PASS


def test_west_missing_is_fail(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    assert doctor._check_west().status == doctor.FAIL


def test_west_old_is_warn(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: "/usr/bin/west")
    monkeypatch.setattr(doctor, "_tool_version", lambda _: (1, 0))
    assert doctor._check_west().status == doctor.WARN


def test_west_recent_is_pass(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: "/usr/bin/west")
    monkeypatch.setattr(doctor, "_tool_version", lambda _: (1, 2))
    assert doctor._check_west().status == doctor.PASS


def test_cmake_old_is_fail(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: "/usr/bin/cmake")
    monkeypatch.setattr(doctor, "_tool_version", lambda _: (3, 10))
    assert doctor._check_cmake().status == doctor.FAIL


def test_python_deps_missing_is_fail(monkeypatch):
    monkeypatch.setattr(
        doctor.importlib.util,
        "find_spec",
        lambda name: None if name == "cbor2" else object(),
    )
    res = doctor._check_python_deps()
    assert res.status == doctor.FAIL
    assert "cbor2" in res.message


def test_host_compiler_missing_is_warn(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    assert doctor._check_host_compiler().status == doctor.WARN


def test_zephyr_base_unset_is_warn(monkeypatch):
    monkeypatch.delenv("ZEPHYR_BASE", raising=False)
    assert doctor._check_zephyr_base().status == doctor.WARN


def test_zephyr_base_bad_is_fail(monkeypatch, tmp_path):
    monkeypatch.setenv("ZEPHYR_BASE", str(tmp_path))  # no VERSION file
    assert doctor._check_zephyr_base().status == doctor.FAIL


def test_zephyr_version_mismatch_is_fail(monkeypatch, tmp_path):
    base = tmp_path / "zephyr"
    base.mkdir()
    (base / "VERSION").write_text(
        "VERSION_MAJOR = 3\nVERSION_MINOR = 7\n", encoding="utf-8"
    )
    monkeypatch.setenv("ZEPHYR_BASE", str(base))
    res = doctor._check_zephyr_version()
    assert res.status == doctor.FAIL
    assert "west update" in (res.hint or "")


def test_zephyr_version_match_is_pass(monkeypatch, tmp_path):
    base = tmp_path / "zephyr"
    base.mkdir()
    (base / "VERSION").write_text(
        "VERSION_MAJOR = 4\nVERSION_MINOR = 4\n", encoding="utf-8"
    )
    monkeypatch.setenv("ZEPHYR_BASE", str(base))
    assert doctor._check_zephyr_version().status == doctor.PASS


def test_west_workspace_missing_is_fail(monkeypatch, tmp_path):
    base = tmp_path / "zephyr"
    base.mkdir()
    monkeypatch.setenv("ZEPHYR_BASE", str(base))  # no sibling .west
    assert doctor._check_west_workspace().status == doctor.FAIL


def test_workspace_venv_present_not_active_is_warn(monkeypatch, tmp_path):
    base = tmp_path / "zephyr"
    base.mkdir()
    (base.parent / ".venv").mkdir()
    monkeypatch.setenv("ZEPHYR_BASE", str(base))
    monkeypatch.setattr(doctor.sys, "prefix", "/usr")  # not the venv
    assert doctor._check_workspace_venv().status == doctor.WARN


# -------- end-to-end command behaviour ---------------------------------------


def test_doctor_all_pass_exit_zero(monkeypatch, tmp_path):
    _force_all_pass(monkeypatch, tmp_path)
    result = _run(["--no-color"])
    assert result.exit_code == 0, result.output
    assert "[PASS]" in result.output
    assert "[FAIL]" not in result.output


def test_doctor_fail_exits_one(monkeypatch, tmp_path):
    _force_all_pass(monkeypatch, tmp_path)
    # Break cmake -> a FAIL.
    monkeypatch.setattr(
        doctor, "_tool_version",
        lambda args: (3, 10) if args[0] == "cmake" else (9, 9),
    )
    result = _run(["--no-color"])
    assert result.exit_code == 1
    assert "[FAIL]" in result.output


def test_doctor_strict_promotes_warn(monkeypatch, tmp_path):
    _force_all_pass(monkeypatch, tmp_path)
    # Force a WARN: no host compiler.
    real_which = doctor.shutil.which

    def _which(name):
        if name in ("gcc", "clang"):
            return None
        return real_which(name)

    monkeypatch.setattr(doctor.shutil, "which", _which)

    # Without --strict, a lone WARN must still exit 0.
    assert _run(["--no-color"]).exit_code == 0
    # With --strict, the WARN promotes to a nonzero exit.
    assert _run(["--no-color", "--strict"]).exit_code == 1


def test_doctor_json_is_machine_readable(monkeypatch, tmp_path):
    _force_all_pass(monkeypatch, tmp_path)
    result = _run(["--json"])
    assert result.exit_code == 0
    payload = json.loads(result.output)
    assert payload["ok"] is True
    assert payload["summary"]["fail"] == 0
    names = {c["name"] for c in payload["checks"]}
    assert {"python", "west", "cmake", "zephyr-version"} <= names


def test_doctor_registered_in_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "doctor" in result.output
