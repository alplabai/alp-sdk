"""Tests for `alp doctor` -- the read-only environment preflight.

Everything here monkeypatches the probing surface (shutil.which, the env,
sys.version_info, the `_tool_version` shell-out helper) so the tests are
hermetic: they never actually inspect this host's toolchain.
"""

from __future__ import annotations

import json
from collections import namedtuple

import pytest
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

    # Stub python-deps PASS directly rather than relying on the host venv
    # actually having every optional dep (e.g. questionary) installed; the
    # FAIL branch is covered hermetically by test_python_deps_missing_is_fail.
    monkeypatch.setattr(
        doctor,
        "_check_python_deps",
        lambda: doctor.CheckResult("python-deps", doctor.PASS, "all deps present"),
    )

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


def test_python_pin_mismatch_is_warn(monkeypatch, tmp_path):
    (tmp_path / ".python-version").write_text("3.12\n", encoding="utf-8")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(doctor.sys, "version_info", _VInfo(3, 11, 4, "final", 0))
    res = doctor._check_python_pin()
    assert res is not None
    assert res.status == doctor.WARN
    assert "3.11.4" in res.message and "3.12" in res.message
    # The floor/pin distinction must be spelled out in the hint.
    assert "3.10" in (res.hint or "")


def test_python_pin_match_is_pass(monkeypatch, tmp_path):
    (tmp_path / ".python-version").write_text("3.12\n", encoding="utf-8")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    monkeypatch.setattr(doctor.sys, "version_info", _VInfo(3, 12, 3, "final", 0))
    res = doctor._check_python_pin()
    assert res is not None
    assert res.status == doctor.PASS


def test_python_pin_absent_skips_check(monkeypatch, tmp_path):
    # Packaged install without the repo checkout: no .python-version -> the
    # check is skipped entirely rather than guessing a pin.
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    assert doctor._check_python_pin() is None


def test_python_pin_unparseable_is_warn(monkeypatch, tmp_path):
    (tmp_path / ".python-version").write_text("bogus\n", encoding="utf-8")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    res = doctor._check_python_pin()
    assert res is not None
    assert res.status == doctor.WARN


def test_python_pin_in_all_checks_when_pinned(monkeypatch, tmp_path):
    _force_all_pass(monkeypatch, tmp_path)
    # _force_all_pass points _repo_root at tmp_path; add a matching pin so
    # the check appears (and PASSes) in the aggregate --json report.
    v = doctor.sys.version_info
    (tmp_path / ".python-version").write_text(
        f"{v.major}.{v.minor}\n", encoding="utf-8"
    )
    result = _run(["--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    by_name = {c["name"]: c for c in payload["checks"]}
    assert by_name["python-pin"]["status"] == "PASS"
    # Ordering contract: the pin line reads right after the floor line.
    names = [c["name"] for c in payload["checks"]]
    assert names.index("python-pin") == names.index("python") + 1


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


def test_cmake_missing_hint_windows_sourced_from_bootstrap_manifest(monkeypatch):
    """cmake is tracked in BOTH prerequisites.posix and prerequisites.windows
    -- unlike ninja (windows-only), routing its hints through
    _prereq_install_hint gives all three install.{linux,macos,windows} OS
    maps a real run-time reader (issue #949 review, minor 5)."""
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    monkeypatch.setattr(doctor.sys, "platform", "win32")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install it: winget install -e --id Kitware.CMake."


def test_cmake_missing_hint_linux_sourced_from_bootstrap_manifest(monkeypatch):
    # cmake itself is absent, but apt-get resolves (a Debian/Ubuntu host) --
    # issue #1464's PM-detection hop (_prereq_linux_pm) needs a REAL PM
    # binary to distinguish this from "no known package manager at all";
    # a blanket which()->None stub can no longer stand in for both.
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/apt-get" if name == "apt-get" else None,
    )
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install it: sudo apt-get install -y cmake."


def test_cmake_missing_hint_linux_dnf_sourced_from_bootstrap_manifest(monkeypatch):
    """Same shape, dnf-family host (issue #1464): apt-get absent, dnf
    present -- the hint must be the real dnf command, not the apt one."""
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/dnf" if name == "dnf" else None,
    )
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install it: sudo dnf install -y cmake."


def test_cmake_missing_hint_linux_no_known_pm_falls_back_to_generic(monkeypatch):
    """Neither apt-get nor dnf resolves (e.g. Arch/Alpine -- this manifest
    ships no install.linux.pacman entry at all, issue #1464) -- must degrade
    to the generic, package-manager-agnostic hint, never guess."""
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install cmake via your OS package manager."


def test_cmake_missing_hint_macos_sourced_from_bootstrap_manifest(monkeypatch):
    """`install.macos` had ZERO test coverage before this: `_prereq_os_key()`
    does reach `prerequisites.install.macos` at run time on a real darwin
    host, but no test ever monkeypatched `sys.platform = "darwin"`, so a
    hostile value there (verified: `install.macos.cmake = "rm -rf /"`)
    shipped green through the gate AND the doctor tests both."""
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    monkeypatch.setattr(doctor.sys, "platform", "darwin")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install it: brew install cmake."


def test_cmake_old_hint_sourced_from_bootstrap_manifest(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: "/usr/bin/cmake")
    monkeypatch.setattr(doctor, "_tool_version", lambda _: (3, 10))
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_cmake()
    assert result.status == doctor.FAIL
    assert result.hint == "Install it: sudo apt-get install -y cmake."


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


def test_ninja_missing_is_fail(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    assert doctor._check_ninja().status == doctor.FAIL


def test_ninja_missing_hint_windows_sourced_from_bootstrap_manifest(monkeypatch):
    """The remediation hint is read from metadata/bootstrap.json's
    prerequisites.install (issue #949), not hardcoded -- regression for the
    shipped drift (a winget ID missing `-e --id`, and an apt form matching
    no canonical command anywhere else)."""
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    monkeypatch.setattr(doctor.sys, "platform", "win32")
    result = doctor._check_ninja()
    assert result.hint == "Install it: winget install -e --id Ninja-build.Ninja."


def test_ninja_missing_hint_posix_has_real_command(monkeypatch):
    """ninja is one of prerequisites.posix's tracked tools (issue #971) --
    the hint must source the real apt/brew command from
    metadata/bootstrap.json's prerequisites.install.linux.apt, not fall back
    to the generic pointer."""
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/apt-get" if name == "apt-get" else None,
    )
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_ninja()
    assert result.hint == "Install it: sudo apt-get install -y ninja-build."


def test_ninja_missing_hint_linux_dnf_falls_back_to_generic(monkeypatch):
    """dnf resolves, but install.linux.dnf carries no `ninja` key (issue
    #1464 -- unshippable as one dnf-ecosystem-uniform command: Fedora's own
    repos carry ninja-build, RHEL-derivatives don't without EPEL). Must
    degrade to the generic hint, never a guessed dnf command."""
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/dnf" if name == "dnf" else None,
    )
    monkeypatch.setattr(doctor.sys, "platform", "linux")
    result = doctor._check_ninja()
    assert result.hint == "Install ninja via your OS package manager."


# -------- malformed metadata/bootstrap.json: must degrade, never crash -------
#
# A truncated or bad-merge bootstrap.json is exactly the broken workspace
# `alp doctor` gets run on -- these lock the read/parse contract
# `_bootstrap_manifest`'s own docstring promises (issue #949 review): every
# shape below used to raise AttributeError/UnicodeDecodeError out of
# `_prereq_install_hint` instead of falling back to the generic hint.


def _write_manifest(tmp_path, content_bytes):
    (tmp_path / "metadata").mkdir()
    (tmp_path / "metadata" / "bootstrap.json").write_bytes(content_bytes)


def test_bootstrap_manifest_top_level_list_returns_none(monkeypatch, tmp_path):
    _write_manifest(tmp_path, b"[]")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    assert doctor._bootstrap_manifest() is None


def test_bootstrap_manifest_top_level_string_returns_none(monkeypatch, tmp_path):
    _write_manifest(tmp_path, b'"nope"')
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    assert doctor._bootstrap_manifest() is None


def test_bootstrap_manifest_invalid_utf8_returns_none(monkeypatch, tmp_path):
    _write_manifest(tmp_path, b"\xff\xfe\x00bad")
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    assert doctor._bootstrap_manifest() is None


@pytest.mark.parametrize(
    "content",
    [
        b"[]",
        b'"nope"',
        b'{"prerequisites": []}',
        b'{"prerequisites": {"install": null}}',
        b"\xff\xfe\x00bad",
        # Every prior case bails out at one of the four dict hops
        # (`prerequisites`/`install`/`<os>`/`<tool>`) BEFORE reaching a leaf
        # -- none of them exercise `isinstance(node, str)` itself, so it
        # survives mutation to the truthiness-only `if node:` (both 123 and
        # {} are truthy, so a `123.` or `{'a': 1}.` non-string leaf would
        # still print as though it were a real command instead of falling
        # back). All three OS keys carry the same leaf value so this fires
        # regardless of which OS runs the test.
        b'{"prerequisites": {"install": {"linux": {"ninja": 123}, '
        b'"macos": {"ninja": 123}, "windows": {"ninja": 123}}}}',
        b'{"prerequisites": {"install": {"linux": {"ninja": {}}, '
        b'"macos": {"ninja": {}}, "windows": {"ninja": {}}}}}',
    ],
    ids=[
        "top-level-list",
        "top-level-string",
        "prerequisites-not-dict",
        "install-null",
        "invalid-utf8",
        "leaf-int",
        "leaf-dict",
    ],
)
def test_prereq_install_hint_falls_back_on_malformed_manifest(monkeypatch, tmp_path, content):
    _write_manifest(tmp_path, content)
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    result = doctor._prereq_install_hint("ninja")
    assert result == "Install ninja via your OS package manager."


def test_dtc_and_gperf_missing_are_warn_only(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    dtc_result = doctor._check_dtc()
    gperf_result = doctor._check_gperf()
    assert dtc_result.status == doctor.WARN
    assert gperf_result.status == doctor.WARN
    for result in (dtc_result, gperf_result):
        # issue #949: this hint used to claim the tool was "bundled with the
        # Zephyr SDK on Windows" -- false, and a customer following it would
        # install the SDK and still not have the tool. Pin both the absence
        # of the old false claim and the presence of the corrective text so
        # a regression here fails a test instead of shipping silently.
        assert "bundled with the Zephyr SDK on Windows" not in result.hint
        assert "does not ship" in result.hint
        assert "cross-platform-setup.md" in result.hint


def test_jlink_missing_is_warn_only(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    assert doctor._check_jlink().status == doctor.WARN


def test_jlink_present_is_pass(monkeypatch):
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/JLinkExe" if name == "JLinkExe" else None,
    )
    assert doctor._check_jlink().status == doctor.PASS


def test_tan_missing_is_warn_only(monkeypatch):
    monkeypatch.setattr(doctor.shutil, "which", lambda _: None)
    assert doctor._check_tan().status == doctor.WARN


def test_tan_present_is_pass(monkeypatch):
    monkeypatch.setattr(
        doctor.shutil, "which",
        lambda name: "/usr/bin/tan" if name == "tan" else None,
    )
    assert doctor._check_tan().status == doctor.PASS


def test_zephyr_pin_read_live_from_west_yml(monkeypatch, tmp_path):
    (tmp_path / "west.yml").write_text(
        "manifest:\n"
        "  projects:\n"
        "    - name: zephyr\n"
        "      revision: v9.9.9\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)
    assert doctor._zephyr_pin() == "v9.9.9"
    assert doctor._pin_mm() == (9, 9)


def test_zephyr_pin_falls_back_without_west_yml(monkeypatch, tmp_path):
    monkeypatch.setattr(doctor, "_repo_root", lambda: tmp_path)  # no west.yml
    assert doctor._zephyr_pin() == doctor.ZEPHYR_PIN


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
