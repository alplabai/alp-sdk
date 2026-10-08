# SPDX-License-Identifier: Apache-2.0
"""board.yaml `cameras:` -> Zephyr `-DSHIELD` / Yocto `ALP_CAMERA_CAM<n>`
(scripts/alp_orchestrate/cameras.py)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import emit_build_plan, load_board_yaml  # noqa: E402

AEN = """
    som:
      sku: E1M-AEN803
    preset: e1m-evk
    cores:
      a32_cluster:
        os: "off"
      m55_he:
        app: ./src
    cameras:
      - {{ connector: CAM0, module: {module} }}
"""

V2M = """
    som:
      sku: E1M-V2M101
    preset: e1m-x-evk
    cores:
      a55_cluster:
        image: alp-image-edge
    cameras:
      - {{ connector: CAM0, module: {module} }}
"""


def _plan(tmp_path: Path, body: str, module: str) -> dict:
    path = _write_board(tmp_path, body.format(module=module))
    return json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))


def _slice(plan: dict, core: str) -> dict:
    return next(s for s in plan["slices"] if s["coreId"] == core)


def _artefact(slice_: dict, name: str) -> dict:
    return next(c for c in slice_["configArtefacts"]
                if c["path"].endswith("/" + name))


def test_aen_zephyr_shield_define_and_cmake_args(tmp_path: Path) -> None:
    plan = _plan(tmp_path, AEN, "innomaker_cam_ov9281")
    shield = "e1m_evk_rpi_csi innomaker_cam_ov9281"
    he = _slice(plan, "m55_he")
    assert f"-DSHIELD={shield}" in he["command"]["args"]
    assert not plan["warnings"]
    # the stock-shim HP core is not a camera owner
    assert not any(a.startswith("-DSHIELD") for a in
                   _slice(plan, "m55_hp")["command"]["args"])
    args_txt = _artefact(he, "cmake-args.txt")
    assert f"-DSHIELD={shield}" in args_txt["contents"]


def test_aen_module_without_zephyr_shield_blocks_command(tmp_path: Path) -> None:
    plan = _plan(tmp_path, AEN, "raspberry_pi_camera_module_2")
    assert _slice(plan, "m55_he")["command"] is None
    w = next(w for w in plan["warnings"] if w["code"] == "camera-select-failed")
    assert "raspberry_pi_camera_module_2" in w["message"]
    assert "zephyr_shield" in w["message"]


def test_missing_board_overlay_blocks_command(tmp_path: Path, monkeypatch) -> None:
    from alp_orchestrate import cameras
    monkeypatch.setattr(cameras, "_has_overlay", lambda *a: False)
    plan = _plan(tmp_path, AEN, "innomaker_cam_ov9281")
    w = next(w for w in plan["warnings"] if w["code"] == "camera-select-failed")
    assert "no Zephyr shield overlay" in w["message"]


def test_v2m_yocto_local_conf(tmp_path: Path) -> None:
    plan = _plan(tmp_path, V2M, "raspberry_pi_global_shutter_camera")
    a55 = _slice(plan, "a55_cluster")
    assert ('ALP_CAMERA_CAM0 = "raspberry_pi_global_shutter_camera"'
            in _artefact(a55, "local.conf")["contents"])
    # CM33 stock shim gets no SHIELD, no error
    assert not any(a.startswith("-DSHIELD") for a in
                   (_slice(plan, "m33_sm")["command"] or {"args": []})["args"])
    assert not plan["warnings"]
