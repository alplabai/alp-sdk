# SPDX-License-Identifier: Apache-2.0
"""scripts/check_camera_parity.py (#2633): the real tree is clean, and each
seeded drift is reported by name."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_camera_parity as gate  # noqa: E402
import gen_camera_dt as gen  # noqa: E402

_COPY = ("metadata/boards", "metadata/camera_modules", "metadata/chips",
         "metadata/e1m_modules", "metadata/socs/renesas", "metadata/os",
         "meta-alp-sdk/recipes-kernel/linux/linux-renesas", "docs")


def _only_md(directory, names):
    return [n for n in names if (Path(directory) / n).is_file() and not n.endswith(".md")]


@pytest.fixture(scope="module")
def _base(tmp_path_factory):
    root = tmp_path_factory.mktemp("paritytree")
    for d in _COPY:
        shutil.copytree(REPO / d, root / d, ignore=_only_md if d == "docs" else None)
    return root


@pytest.fixture
def tree(_base, tmp_path):
    root = tmp_path / "t"
    shutil.copytree(_base, root)
    return root


def _edit(path: Path, fn) -> None:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    fn(doc)
    path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")


def test_real_tree_is_clean():
    assert gate.find_problems(REPO) == []


def test_clean_copy_passes(tree):
    assert gate.find_problems(tree) == []


def test_address_lanes_and_xclk_must_fit_the_chip(tree):
    _edit(tree / "metadata/camera_modules/innomaker_cam_ov9281.yaml",
          lambda d: d.update(i2c_addr_7bit=0x11, lanes=4, xclk_hz=25000000))
    msg = "\n".join(gate.find_problems(tree))
    assert "i2c_addr_7bit 0x11 not in chip ov9281 i2c.addresses ['0x60', '0x62']" in msg
    assert "lanes 4 not in chip ov9281 mipi.lanes_supported [1, 2]" in msg
    assert "4 lanes fit no carrier camera connector" in msg
    assert "xclk_hz 25000000 not in chip ov9281 drivers.linux.xclk_supported_hz [24000000]" in msg


def test_missing_linux_driver_facts_are_listed(tree):
    def strip(d):
        d["drivers"]["linux"].pop("kconfig")
        d["drivers"]["linux"]["link_freqs"] = [{"lanes": 1, "link_freqs_hz": [1]}]
    _edit(tree / "metadata/chips/ov9281.yaml", strip)
    msg = "\n".join(gate.find_problems(tree))
    assert "drivers.linux.kconfig is missing" in msg
    assert "link_freqs has no entry for 2 lanes" in msg


def test_driver_must_exist_in_the_bsp_kernel(tree):
    _edit(tree / "metadata/chips/ov9281.yaml",
          lambda d: d["drivers"]["linux"].update(compatible="acme,nothing"))
    assert "no BSP kernel driver for acme,nothing" in "\n".join(gate.find_problems(tree))


def test_named_kernel_patch_must_exist(tree):
    (tree / gen.LINUX_DIR / "0020-media-i2c-ov9282-add-1280x800-and-640x400-modes.patch").unlink()
    assert "kernel patch 0020-media-i2c-ov9282" in "\n".join(gate.find_problems(tree))


def test_kconfig_must_be_in_the_generated_cfg(tree):
    cfg = tree / gen.LINUX_DIR / gen.CFG_NAME
    cfg.write_text(cfg.read_text(encoding="utf-8").replace("CONFIG_VIDEO_OV9282=y\n", ""),
                   encoding="utf-8")
    assert "CONFIG_VIDEO_OV9282 is not =y" in "\n".join(gate.find_problems(tree))


def test_docs_knob_value_must_be_a_generated_fragment(tree):
    (tree / "docs/zz-camera.md").write_text('ALP_CAMERA_CAM0 = "no_such_camera"\n', encoding="utf-8")
    msg = "\n".join(gate.find_problems(tree))
    assert 'ALP_CAMERA_CAM0 = "no_such_camera" has no generated fragment' in msg
    assert "innomaker_cam_ov9281" in msg


def test_bench_verified_sku_must_exist(tree):
    _edit(tree / "metadata/camera_modules/innomaker_cam_ov9281.yaml",
          lambda d: d.update(linux_bench_verified_on="E1M-NOPE1"))
    assert "linux_bench_verified_on E1M-NOPE1 is not a SoM" in "; ".join(gate.find_problems(tree))
