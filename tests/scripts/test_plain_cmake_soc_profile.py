# SPDX-License-Identifier: Apache-2.0
"""The SoC capability profile reaches plain-CMake (baremetal/Yocto) builds.

Plain CMake has no Kconfig, so <alp/soc_caps.h> used to see no
CONFIG_ALP_SOC_* and fall to the permissive "unknown" branch
(ALP_SOC_REF_STR "unknown", every ALP_HAS() true) -- silicon_ref-exact
backends never matched.  The loader now emits `-DALP_SOC=<symbol>` /
`ALP_SDK_SOC` and the top-level CMakeLists.txt turns it into
`CONFIG_<symbol>=1` on the public alp_sdk_headers target.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import REPO, _run_loader, _write_board  # noqa: E402

_V2N_BOARD = """
    som:
      sku: E1M-V2N101
    cores:
      a55_cluster:
        os: yocto
        image: alp-image-edge
"""
_V2N_SYMBOL = "ALP_SOC_RENESAS_RZV2N_N44"


def _emit(emit: str, core: str) -> str:
    with tempfile.TemporaryDirectory() as td:
        path = _write_board(Path(td), _V2N_BOARD)
        rv = _run_loader(input_path=path, emit=emit, core=core)
    assert rv.returncode == 0, rv.stderr
    return rv.stdout


def test_yocto_local_conf_carries_the_soc_symbol():
    assert f'ALP_SDK_SOC ?= "{_V2N_SYMBOL}"' in _emit("yocto-conf", "a55_cluster").splitlines()


def test_cmake_args_carry_the_soc_symbol_for_a_baremetal_slice():
    with tempfile.TemporaryDirectory() as td:
        path = _write_board(Path(td), """
            som:
              sku: E1M-V2N101
            cores:
              m33_sm:
                os: baremetal
                app: ./m33
        """)
        rv = _run_loader(input_path=path, emit="cmake-args", core="m33_sm")
    assert rv.returncode == 0, rv.stderr
    assert f"-DALP_SOC={_V2N_SYMBOL}" in rv.stdout.splitlines()


def test_recipe_forwards_the_variable_to_cmake():
    bb = (REPO / "meta-alp-sdk/recipes-core/alp-sdk/alp-sdk_git.bb").read_text(encoding="utf-8")
    assert "-DALP_SOC=" in bb
    assert "d.getVar('ALP_SDK_SOC')" in bb


def _compiler() -> str | None:
    for name in ("arm-none-eabi-gcc", "gcc", "cc", "clang"):
        if shutil.which(name):
            return name
    return None


def _configure(tmp: Path, soc: str) -> subprocess.CompletedProcess[str]:
    """Configure the real top-level CMakeLists.txt via add_subdirectory and
    print the INTERFACE compile definitions a consumer inherits."""
    (tmp / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(consumer LANGUAGES C)\n"
        f'add_subdirectory("{REPO.as_posix()}" sdk)\n'
        "get_target_property(d alp_sdk_headers INTERFACE_COMPILE_DEFINITIONS)\n"
        'message(STATUS "ALP_HEADERS_DEFS=[${d}]")\n',
        encoding="utf-8")
    cmd = ["cmake", "-S", str(tmp), "-B", str(tmp / "build"), "-DALP_OS=baremetal",
           "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
           f"-DCMAKE_C_COMPILER={_compiler()}"]
    if soc:
        cmd.append(f"-DALP_SOC={soc}")
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", check=False)


needs_cmake = pytest.mark.skipif(
    shutil.which("cmake") is None or _compiler() is None,
    reason="needs cmake and a C compiler to configure the SDK")


@needs_cmake
def test_alp_soc_defines_config_symbol_for_plain_cmake_consumers(tmp_path):
    rv = _configure(tmp_path, _V2N_SYMBOL)
    assert rv.returncode == 0, rv.stdout + rv.stderr
    assert f"CONFIG_{_V2N_SYMBOL}=1" in rv.stdout


@needs_cmake
def test_no_alp_soc_keeps_the_permissive_profile(tmp_path):
    rv = _configure(tmp_path, "")
    assert rv.returncode == 0, rv.stdout + rv.stderr
    assert "CONFIG_ALP_SOC_" not in rv.stdout.split("ALP_HEADERS_DEFS=")[1]


@needs_cmake
def test_malformed_alp_soc_is_rejected(tmp_path):
    rv = _configure(tmp_path, "renesas:rzv2n:n44")
    assert rv.returncode != 0
    assert "ALP_SOC='renesas:rzv2n:n44' must be a Kconfig-style SoC symbol" in rv.stdout + rv.stderr


@needs_cmake
def test_unknown_but_well_formed_alp_soc_is_rejected(tmp_path):
    rv = _configure(tmp_path, "ALP_SOC_RENESAS_RZV2N")
    assert rv.returncode != 0
    assert "ALP_SOC='ALP_SOC_RENESAS_RZV2N' is not a SoC profile" in rv.stdout + rv.stderr


def test_machine_confs_default_alp_sdk_soc_to_the_preset_silicon():
    """Plain `MACHINE=... bitbake` (no generated local.conf) must still get
    the SoC profile; the conf default and the emitter share one source."""
    import re

    import yaml

    from alp_project_loader import silicon_to_kconfig

    metadata = REPO / "metadata"
    confs = sorted((REPO / "meta-alp-sdk/conf/machine").glob("e1m-*.conf"))
    assert len(confs) == 8
    for conf in confs:
        sku = conf.stem.rsplit("-", 1)[0].upper()  # e1m-v2n101 -> E1M-V2N101
        preset = yaml.safe_load((metadata / "e1m_modules" / f"{sku}.yaml").read_text(encoding="utf-8"))
        want = silicon_to_kconfig(preset["silicon"], metadata)
        m = re.search(r'^ALP_SDK_SOC \?= "([^"]+)"', conf.read_text(encoding="utf-8"), re.M)
        assert m, f"{conf.name}: no ALP_SDK_SOC ?= default"
        assert m.group(1) == want, conf.name
