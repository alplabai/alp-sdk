# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/flash_backends/ (Wave 5B of the 2026-05-15
heterogeneous-OS orchestration design).  ADR-0020 Phase 4 (preview)
retired the `alp_flash.py` dispatcher this file used to also cover
(section 7) -- the backends themselves stay in scope.

Every test stubs ``subprocess.run`` + ``shutil.which`` so the suite
runs cleanly on Windows without any real flashing tools installed.

Run locally:

    python -m pytest tests/scripts/test_flash_backends.py -v
"""

from __future__ import annotations

import os
import stat
import sys
import types
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import flash_backends                              # noqa: E402,F401
from flash_backends import (                       # noqa: E402
    FlashContext,
    REGISTRY,
    lookup,
)
from flash_backends.baremetal_cmake_flash import (  # noqa: E402
    BaremetalCmakeFlash,
)
from flash_backends import swd_probe                # noqa: E402
from flash_backends.swd_probe import (             # noqa: E402
    SwdProbeFlash,
    _jlink_commander_script,
)
from flash_backends import yocto_wic                # noqa: E402
from flash_backends.yocto_wic import (             # noqa: E402
    YoctoWicFlash,
    _require_block_device,
    _resolve_dev_root,
)
from flash_backends.zephyr_west_flash import (     # noqa: E402
    ZephyrWestFlash,
)


# A fake `os.stat_result`-shaped block device -- used to satisfy the
# resolved-target block-device check in tests that don't exercise the
# check itself (they mock it out so /dev/sdb-style fixture targets that
# don't exist on the test runner still resolve as "valid").
_FAKE_BLOCK_STAT = SimpleNamespace(st_mode=stat.S_IFBLK | 0o660)


def _patch_valid_dev_target():
    """Bypass both of yocto_wic's target-validation gates (root check +
    block-device check) for tests that exercise downstream
    tool-selection/pipeline logic, not the target-validation logic
    itself (that has its own dedicated tests below)."""
    return patch.multiple(
        "flash_backends.yocto_wic",
        _resolve_dev_root=lambda target: (Path(target), None),
        _require_block_device=lambda resolved: None,
    )


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _proc(rc: int = 0, stdout: str = "", stderr: str = "") -> types.SimpleNamespace:
    """Build a CompletedProcess-shaped mock."""
    return SimpleNamespace(returncode=rc, stdout=stdout, stderr=stderr)


def _ctx(method_args: dict | None = None,
         dry_run: bool = False,
         core_id: str = "test_core",
         artefact: str = "/tmp/dummy.bin",
         sku: str = "E1M-V2N101") -> FlashContext:
    return FlashContext(
        artefact_path=Path(artefact),
        flash_args=dict(method_args or {}),
        core_id=core_id,
        sku=sku,
        dry_run=dry_run,
    )


# ---------------------------------------------------------------------
# 1. Registry
# ---------------------------------------------------------------------


def test_registry_has_all_canonical_backends() -> None:
    expected = {
        "yocto_wic_to_sd_or_emmc",
        "yocto_wic",                    # alias
        "zephyr_west_flash",
        "baremetal_cmake_flash",
        "swd_probe",
    }
    assert expected.issubset(set(REGISTRY.keys())), \
        f"missing keys: {expected - set(REGISTRY.keys())}"


def test_lookup_returns_zephyr_instance() -> None:
    backend = lookup("zephyr_west_flash")
    assert isinstance(backend, ZephyrWestFlash)
    assert backend.name == "zephyr_west_flash"
    assert "west" in backend.requires


def test_lookup_returns_none_for_unknown() -> None:
    assert lookup("does_not_exist") is None


# ---------------------------------------------------------------------
# 2. YoctoWicFlash
# ---------------------------------------------------------------------


def test_yocto_wic_dry_run_uses_bmaptool() -> None:
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}" if t == "bmaptool"
               else None), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            dry_run=True,
            artefact="/tmp/image.wic",
        ))
    assert result.ok is True
    assert run_mock.call_count == 0
    assert "bmaptool" in " ".join(result.command)
    assert "/dev/sdb" in " ".join(result.command)


def test_yocto_wic_happy_path_invokes_bmaptool() -> None:
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               side_effect=lambda t: "/usr/bin/bmaptool"
               if t == "bmaptool" else None), \
         patch("flash_backends.yocto_wic.subprocess.run",
               return_value=_proc(rc=0, stdout="ok")) as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            artefact="/tmp/image.wic",
        ))
    assert result.ok is True
    assert run_mock.call_count == 1
    invoked_cmd = run_mock.call_args[0][0]
    assert invoked_cmd[0].endswith("bmaptool")
    assert "/dev/sdb" in invoked_cmd


def test_yocto_wic_falls_back_to_dd_when_bmaptool_absent() -> None:
    backend = YoctoWicFlash()
    artefact = "/tmp/image.wic"
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               side_effect=lambda t: "/bin/dd" if t == "dd" else None), \
         patch("flash_backends.yocto_wic.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            artefact=artefact,
        ))
    assert result.ok is True
    invoked_cmd = run_mock.call_args[0][0]
    joined = " ".join(invoked_cmd)
    assert "dd" in joined
    # On Windows Path() normalises forward slashes to backslashes; the
    # artefact path token therefore appears as the Path-rendered form.
    artefact_token = str(Path(artefact))
    assert f"if={artefact_token}" in joined
    assert "of=/dev/sdb" in joined


def test_yocto_wic_gzip_fallback_dry_run_keeps_path_as_argv_token() -> None:
    backend = YoctoWicFlash()
    artefact = "/tmp/image with spaces; touch nope.wic.gz"
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               side_effect=lambda t: {
                   "dd": "/bin/dd",
                   "gunzip": "/usr/bin/gunzip",
               }.get(t)), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            dry_run=True,
            artefact=artefact,
        ))

    assert result.ok is True
    assert run_mock.call_count == 0
    artefact_token = str(Path(artefact))
    assert result.command[:3] == ["/usr/bin/gunzip", "-c", artefact_token]
    assert "|" in result.command
    assert "sh" not in result.command
    assert artefact_token in result.command


def test_yocto_wic_xz_fallback_streams_without_shell() -> None:
    backend = YoctoWicFlash()
    artefact = "/tmp/image; touch nope.wic.xz"
    bs = "4M; touch nope"
    popen_calls: list[list[str]] = []

    class _FakeStdout:
        def close(self) -> None:
            pass

    class _FakePopen:
        def __init__(self, cmd, **_kwargs):
            popen_calls.append(list(cmd))
            self.stdout = _FakeStdout()

        def wait(self) -> int:
            return 0

    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               side_effect=lambda t: {
                   "dd": "/bin/dd",
                   "xz": "/usr/bin/xz",
               }.get(t)), \
         patch("flash_backends.yocto_wic.subprocess.Popen",
               _FakePopen), \
         patch("flash_backends.yocto_wic.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True, "bs": bs},
            artefact=artefact,
        ))

    assert result.ok is True
    artefact_token = str(Path(artefact))
    assert popen_calls == [["/usr/bin/xz", "-dc", artefact_token]]
    invoked_cmd = run_mock.call_args[0][0]
    assert invoked_cmd == [
        "/bin/dd",
        "of=/dev/sdb",
        f"bs={bs}",
        "conv=fsync",
        "status=progress",
    ]
    assert "sh" not in result.command
    assert artefact_token in result.command
    assert f"bs={bs}" in result.command


def test_yocto_wic_refuses_non_dev_target() -> None:
    backend = YoctoWicFlash()
    with patch("flash_backends.yocto_wic.shutil.which",
               return_value="/usr/bin/bmaptool"), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/tmp/file.img", "confirm": True},
            artefact="/tmp/image.wic",
        ))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "/dev/" in result.message


def test_yocto_wic_refuses_traversal_target_before_running_any_tool() -> None:
    """#1112: `/dev/../tmp/foo` must never reach bmaptool/dd."""
    backend = YoctoWicFlash()
    with patch("flash_backends.yocto_wic.shutil.which",
               return_value="/usr/bin/bmaptool"), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/../tmp/spill.img", "confirm": True},
            artefact="/tmp/image.wic",
        ))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "/dev/" in result.message


def test_resolve_dev_root_rejects_traversal(monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(yocto_wic, "_DEV_ROOT", str(tmp_path))
    escape_target = str(tmp_path / ".." / "escaped.img")
    resolved, err = _resolve_dev_root(escape_target)
    assert resolved is None
    assert err is not None
    assert "not beneath" in err


def test_resolve_dev_root_accepts_a_not_yet_present_target(monkeypatch, tmp_path) -> None:
    """The root check alone must not require the target to exist -- a
    dry-run preview of a device that isn't plugged in yet still needs
    to resolve cleanly."""
    monkeypatch.setattr(yocto_wic, "_DEV_ROOT", str(tmp_path))
    target = tmp_path / "not-plugged-in-yet"
    resolved, err = _resolve_dev_root(str(target))
    assert err is None
    assert resolved == Path(os.path.realpath(str(target)))


def test_resolve_dev_root_rejects_symlink_escape(monkeypatch, tmp_path) -> None:
    dev_root = tmp_path / "dev"
    dev_root.mkdir()
    outside = tmp_path / "outside.img"
    outside.write_bytes(b"not a device")
    link = dev_root / "sdb"
    try:
        link.symlink_to(outside)
    except OSError:
        pytest.skip("symlink creation unsupported on this host")
    monkeypatch.setattr(yocto_wic, "_DEV_ROOT", str(dev_root))
    resolved, err = _resolve_dev_root(str(link))
    assert resolved is None
    assert err is not None
    assert "not beneath" in err


def test_require_block_device_rejects_regular_file(tmp_path) -> None:
    target = tmp_path / "sdb"
    target.write_bytes(b"not a device")
    err = _require_block_device(target)
    assert err is not None
    assert "not a block device" in err


def test_require_block_device_rejects_missing_target(tmp_path) -> None:
    err = _require_block_device(tmp_path / "does-not-exist")
    assert err is not None
    assert "cannot stat" in err


def test_require_block_device_accepts_valid_block_device(tmp_path) -> None:
    target = tmp_path / "sdb"
    with patch("flash_backends.yocto_wic.os.stat",
               return_value=_FAKE_BLOCK_STAT):
        err = _require_block_device(target)
    assert err is None


def test_yocto_wic_execution_rejects_target_that_is_a_regular_file(
    tmp_path,
) -> None:
    """End-to-end: confirm=True + a real (non-block) file target must
    be rejected right before invoking bmaptool, even though the target
    resolves cleanly under the (monkeypatched) dev root."""
    backend = YoctoWicFlash()
    target = tmp_path / "sdb"
    target.write_bytes(b"not a device")
    with patch.object(yocto_wic, "_DEV_ROOT", str(tmp_path)), \
         patch("flash_backends.yocto_wic.shutil.which",
               return_value="/usr/bin/bmaptool"), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": str(target), "confirm": True},
            artefact="/tmp/image.wic",
        ))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "not a block device" in result.message


def test_yocto_wic_missing_tool_returns_clear_error() -> None:
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which", return_value=None), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            artefact="/tmp/image.wic",
        ))
    assert result.ok is False
    assert run_mock.call_count == 0
    msg = result.message.lower()
    assert "bmaptool" in msg
    assert "dd" in msg


def test_yocto_wic_dry_run_without_tools_still_plans() -> None:
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which", return_value=None), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb", "confirm": True},
            dry_run=True,
            artefact="/tmp/image.wic.gz",
        ))

    assert result.ok is True
    assert run_mock.call_count == 0
    assert result.command == [
        "bmaptool", "copy", str(Path("/tmp/image.wic.gz")), "/dev/sdb"
    ]
    assert "dry-run" in result.message


def test_yocto_wic_no_confirm_without_tools_still_plans() -> None:
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which", return_value=None), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb"},
            dry_run=False,
            artefact="/tmp/image.wic",
        ))

    assert result.ok is True
    assert run_mock.call_count == 0
    assert result.command == [
        "bmaptool", "copy", str(Path("/tmp/image.wic")), "/dev/sdb"
    ]
    assert "confirm" in result.message.lower()


def test_yocto_wic_without_confirm_dry_runs_even_when_dry_run_false() -> None:
    """Safety check: without explicit confirm flag, the backend
    should NOT actually run subprocess even if dry_run is False."""
    backend = YoctoWicFlash()
    with _patch_valid_dev_target(), \
         patch("flash_backends.yocto_wic.shutil.which",
               return_value="/usr/bin/bmaptool"), \
         patch("flash_backends.yocto_wic.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"target": "/dev/sdb"},          # confirm omitted
            dry_run=False,
            artefact="/tmp/image.wic",
        ))
    assert result.ok is True                   # dry-runs, doesn't fail
    assert run_mock.call_count == 0
    assert "confirm" in result.message.lower()


# ---------------------------------------------------------------------
# 3. ZephyrWestFlash
# ---------------------------------------------------------------------


def test_zephyr_west_flash_dry_run() -> None:
    backend = ZephyrWestFlash()
    with patch("flash_backends.zephyr_west_flash.shutil.which",
               return_value="/usr/bin/west"), \
         patch("flash_backends.zephyr_west_flash.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"runner": "openocd", "build_dir": "/tmp/build/m33"},
            dry_run=True,
            artefact="/tmp/build/m33/zephyr/zephyr.elf",
        ))
    assert result.ok is True
    assert run_mock.call_count == 0
    joined = " ".join(result.command)
    assert "west flash" in joined
    assert "--runner openocd" in joined
    assert "--build-dir /tmp/build/m33" in joined


def test_zephyr_west_flash_happy_path() -> None:
    backend = ZephyrWestFlash()
    with patch("flash_backends.zephyr_west_flash.shutil.which",
               return_value="/usr/bin/west"), \
         patch("flash_backends.zephyr_west_flash.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"runner": "jlink", "build_dir": "/build/m33"},
            artefact="/build/m33/zephyr/zephyr.elf",
        ))
    assert result.ok is True
    invoked = run_mock.call_args[0][0]
    assert invoked[0].endswith("west")
    assert "--runner" in invoked
    assert "jlink" in invoked


def test_zephyr_west_flash_missing_tool() -> None:
    backend = ZephyrWestFlash()
    with patch("flash_backends.zephyr_west_flash.shutil.which",
               return_value=None), \
         patch("flash_backends.zephyr_west_flash.subprocess.run") as run_mock:
        result = backend.flash(_ctx({"runner": "openocd"}))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "west" in result.message.lower()


def test_zephyr_west_flash_no_runner_defers_to_board_default() -> None:
    backend = ZephyrWestFlash()
    with patch("flash_backends.zephyr_west_flash.shutil.which",
               return_value="/usr/bin/west"), \
         patch("flash_backends.zephyr_west_flash.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx({}))     # no runner
    assert result.ok is True
    assert run_mock.call_count == 1
    invoked = run_mock.call_args[0][0]
    assert "--runner" not in invoked


# ---------------------------------------------------------------------
# 4. BaremetalCmakeFlash
# ---------------------------------------------------------------------


def test_baremetal_cmake_flash_dry_run() -> None:
    backend = BaremetalCmakeFlash()
    with patch("flash_backends.baremetal_cmake_flash.shutil.which",
               return_value="/usr/bin/cmake"), \
         patch("flash_backends.baremetal_cmake_flash.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"build_dir": "/tmp/build/bm", "target": "program"},
            dry_run=True,
        ))
    assert result.ok is True
    assert run_mock.call_count == 0
    joined = " ".join(result.command)
    assert "--build /tmp/build/bm" in joined
    assert "--target program" in joined


def test_baremetal_cmake_flash_happy_path_default_target() -> None:
    backend = BaremetalCmakeFlash()
    with patch("flash_backends.baremetal_cmake_flash.shutil.which",
               return_value="/usr/bin/cmake"), \
         patch("flash_backends.baremetal_cmake_flash.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx({"build_dir": "/tmp/bm"}))
    assert result.ok is True
    invoked = run_mock.call_args[0][0]
    # default target is "flash"
    assert "flash" in invoked


def test_baremetal_cmake_flash_missing_tool() -> None:
    backend = BaremetalCmakeFlash()
    with patch("flash_backends.baremetal_cmake_flash.shutil.which",
               return_value=None), \
         patch("flash_backends.baremetal_cmake_flash.subprocess.run") as run_mock:
        result = backend.flash(_ctx({"build_dir": "/tmp/bm"}))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "cmake" in result.message.lower()


# ---------------------------------------------------------------------
# 5. SwdProbeFlash
# ---------------------------------------------------------------------


def test_swd_probe_dry_run_uses_openocd() -> None:
    backend = SwdProbeFlash()
    artefact = "/tmp/gd32_bridge.bin"
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}" if t == "openocd"
               else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {
                "interface": "cmsis-dap",
                "target":    "gd32g553",
                "base":      "0x08000000",
            },
            dry_run=True,
            artefact=artefact,
        ))
    assert result.ok is True
    assert run_mock.call_count == 0
    joined = " ".join(result.command)
    assert "openocd" in joined
    assert "interface/cmsis-dap.cfg" in joined
    assert "target/gd32g553.cfg" in joined
    assert "0x08000000" in joined
    # On Windows Path() renders the artefact path with backslashes; the
    # joined command therefore contains the platform-rendered form.
    assert str(Path(artefact)) in joined


def test_swd_probe_happy_path_openocd_call() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}" if t == "openocd"
               else None), \
         patch("flash_backends.swd_probe.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            artefact="/tmp/gd32.bin",
        ))
    assert result.ok is True
    assert run_mock.call_count == 1


def test_swd_probe_falls_back_to_pyocd() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/pyocd"
               if t == "pyocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            artefact="/tmp/gd32.bin",
        ))
    assert result.ok is True
    invoked = run_mock.call_args[0][0]
    assert invoked[0].endswith("pyocd")
    assert "flash" in invoked


def test_swd_probe_missing_both_tools() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               return_value=None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
        ))
    assert result.ok is False
    assert run_mock.call_count == 0
    msg = result.message.lower()
    assert "openocd" in msg or "pyocd" in msg


def test_swd_probe_requires_interface_and_target() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/openocd"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx({}))
    assert result.ok is False
    assert run_mock.call_count == 0


def test_swd_probe_prefers_jlink_when_present() -> None:
    backend = SwdProbeFlash()
    artefact = "/tmp/gd32-bridge.hex"
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t in ("JLinkExe", "openocd") else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"base": "0x08000000"}, dry_run=True, artefact=artefact))
    assert result.ok is True
    assert run_mock.call_count == 0
    assert result.command[0].endswith("JLinkExe")
    assert "-device" in result.command and "GD32G553MEY7TR" in result.command
    assert "SWD" in " ".join(result.command)
    assert "loadfile" in result.message
    assert "gd32-bridge.hex" in result.message


def test_swd_probe_jlink_happy_path() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_probe.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"jlink_device": "GD32G553MEY7TR"}, artefact="/tmp/gd32-bridge.hex"))
    assert result.ok is True
    assert run_mock.call_count == 1
    assert run_mock.call_args[0][0][0].endswith("JLinkExe")


def test_jlink_commander_script_hex_uses_loadfile() -> None:
    script, err = _jlink_commander_script(Path("/tmp/gd32-bridge.hex"),
                                          "0x08000000", do_reset=True)
    assert err is None
    assert "loadfile" in script
    assert "gd32-bridge.hex" in script
    assert "g" in script.split()
    assert script.strip().endswith("qc")


def test_jlink_commander_script_bin_uses_loadbin_with_base() -> None:
    script, err = _jlink_commander_script(Path("/tmp/gd32-bridge.bin"),
                                          "0x08000000", do_reset=True)
    assert err is None
    assert "loadbin" in script
    assert "0x08000000" in script


def test_swd_probe_jlink_failure_returns_error_result() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_probe.subprocess.run",
               return_value=_proc(rc=1, stderr="Cannot connect to target")):
        result = backend.flash(_ctx(artefact="/tmp/gd32-bridge.hex"))
    assert result.ok is False
    assert "J-Link exited" in result.message


def test_swd_probe_use_openocd_skips_jlink() -> None:
    backend = SwdProbeFlash()
    # Both J-Link and openocd present, but use_openocd forces openocd.
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t in ("JLinkExe", "openocd") else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"use_openocd": True, "interface": "cmsis-dap", "target": "gd32g553"},
            dry_run=True))
    assert result.ok is True
    assert run_mock.call_count == 0
    assert result.command[0].endswith("openocd")
    assert "JLinkExe" not in " ".join(result.command)


# ---------------------------------------------------------------------
# 6. #1113 -- swd_probe address parsing + script-argument hardening
# ---------------------------------------------------------------------


def test_swd_probe_rejects_command_separator_in_base() -> None:
    """A Tcl-separator payload in `base` must never reach a generated
    script -- rejected before any tool is invoked."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553",
             "base": "0x08000000; reset run"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "not a valid address" in result.message


def test_swd_probe_rejects_whitespace_in_base() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which", return_value=None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553",
             "base": "0x08000000 0x0"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "not a valid address" in result.message


def test_swd_probe_rejects_out_of_range_base() -> None:
    """A syntactically-valid decimal literal that overflows the 32-bit
    address space (the hex form is bounded by the regex's digit cap;
    the decimal form needs the explicit range check)."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which", return_value=None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553",
             "base": "4294967296"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "out of range" in result.message


def test_swd_probe_jlink_rejects_newline_in_artefact_path() -> None:
    """A newline in the artefact path is what turns one J-Link Commander
    line into two -- must be rejected before the script is written."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock, \
         patch("flash_backends.swd_probe.tempfile.mkstemp") as mkstemp_mock:
        result = backend.flash(_ctx(
            artefact="/tmp/evil.hex\nr\nloadbin /dev/mem, 0x0"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert mkstemp_mock.call_count == 0


def test_swd_probe_jlink_rejects_double_quote_in_artefact_path() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(artefact='/tmp/evil".hex'))
    assert result.ok is False
    assert run_mock.call_count == 0


def test_swd_probe_jlink_handles_whitespace_bearing_path() -> None:
    """A path with spaces must be quoted correctly, not misparsed."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"base": "0x08000000"},
            dry_run=True,
            artefact="/tmp/dir with spaces/gd32-bridge.hex"))
    assert result.ok is True
    assert run_mock.call_count == 0
    # Derive the expectation from the same Path the backend renders, instead
    # of hardcoding the POSIX spelling.  On Windows `str(Path(...))` yields
    # backslashes, and emitting a native path into the generated probe script
    # on a Windows host is CORRECT -- the probe tool is native there.
    # Asserting a literal "/tmp/..." tested the host's path flavour, not the
    # quoting this case exists to check.
    expected = '"' + str(Path("/tmp/dir with spaces/gd32-bridge.hex")) + '"'
    assert expected in result.message


def test_swd_probe_openocd_rejects_control_char_in_artefact_path() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            artefact="/tmp/evil.bin\n; reset run"))
    assert result.ok is False
    assert run_mock.call_count == 0


def test_swd_probe_openocd_rejects_tcl_brace_in_artefact_path() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            artefact="/tmp/evil}; set x [exec rm -rf /]{}.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0


def test_swd_probe_openocd_handles_whitespace_bearing_path() -> None:
    """A path with spaces round-trips through Tcl brace-quoting."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            dry_run=True,
            artefact="/tmp/dir with spaces/gd32.bin"))
    assert result.ok is True
    assert run_mock.call_count == 0
    # Host-derived for the same reason as the case above; the assertion still
    # pins the Tcl brace-quoting, which is the actual subject of this test.
    expected = "{" + str(Path("/tmp/dir with spaces/gd32.bin")) + "}"
    assert expected in " ".join(result.command)


def test_parse_address_accepts_hex_and_decimal() -> None:
    value, err = swd_probe._parse_address("0x08000000")
    assert err is None
    assert value == 0x08000000
    value, err = swd_probe._parse_address("134217728")
    assert err is None
    assert value == 134217728


def test_format_address_is_canonical_regardless_of_input_case() -> None:
    value, _ = swd_probe._parse_address("0X8000000")
    assert swd_probe._format_address(value) == "0x08000000"


def test_parse_address_leading_zero_decimal_does_not_raise() -> None:
    """int(text, 0) treats a leading-zero decimal as ambiguous octal and
    raises; _parse_address must never surface that as an exception."""
    value, err = swd_probe._parse_address("0123")
    assert err is None
    assert value == 123


# ---------------------------------------------------------------------
# 7. adversarial-review fixes: the third swd_probe sink + Tcl edge case
# ---------------------------------------------------------------------


def test_swd_probe_rejects_traversal_in_interface() -> None:
    """The third injection sink: OpenOCD sources a `-f` file as Tcl, so
    `interface`/`target` reaching `-f interface/{interface}.cfg` raw is
    arbitrary Tcl execution, not just an unexpected file read. A
    traversal payload here must be rejected before openocd ever runs --
    this is the case that must fail on the pre-fix code and pass after."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "../../../../tmp/pwn",
             "target": "../../../../tmp/pwn2"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "not a valid config name" in result.message
    # No argv was ever built from the traversal payload.
    assert not any(
        "../" in str(part) for call in run_mock.call_args_list
        for part in (call.args[0] if call.args else [])
    )


def test_swd_probe_rejects_traversal_in_target() -> None:
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "../../../../tmp/pwn2"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is False
    assert run_mock.call_count == 0
    assert "not a valid config name" in result.message


def test_swd_probe_accepts_ordinary_interface_and_target() -> None:
    """The new config-name guard must not reject the ordinary values
    the openocd/pyocd path is documented to take."""
    backend = SwdProbeFlash()
    with patch("flash_backends.swd_probe.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t == "openocd" else None), \
         patch("flash_backends.swd_probe.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"interface": "cmsis-dap", "target": "gd32g553"},
            artefact="/tmp/gd32.bin"))
    assert result.ok is True
    invoked_cmd = run_mock.call_args[0][0]
    assert "interface/cmsis-dap.cfg" in invoked_cmd
    assert "target/gd32g553.cfg" in invoked_cmd


def test_tcl_quote_rejects_trailing_backslash() -> None:
    """A trailing backslash escapes the closing brace Tcl would
    otherwise see, producing an unbalanced word -- reject it rather
    than emit a script that fails with a confusing parse error."""
    assert swd_probe._tcl_quote("/tmp/a\\") is None
    assert swd_probe._tcl_quote("/tmp/a") == "{/tmp/a}"

