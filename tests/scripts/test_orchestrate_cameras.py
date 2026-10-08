# SPDX-License-Identifier: Apache-2.0
"""board.yaml `cameras:` -> Zephyr `-DSHIELD` / Yocto `ALP_CAMERA_CAM<n>`
(scripts/alp_orchestrate/cameras.py, ownership in camera_owner.py)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_cli.validator import validate_board_yaml  # noqa: E402
from alp_orchestrate import camera_owner, emit_build_plan, load_board_yaml  # noqa: E402

CAM = "      - {{ connector: CAM0, module: {module}{core} }}\n"

AEN = """
    som:
      sku: E1M-AEN803
    preset: e1m-evk
    cores:
      a32_cluster:
        os: "off"
      m55_he:
        app: ./src
    {extra}
    cameras:
""" + CAM

AEN_HE_HP = AEN.replace("      m55_he:\n        app: ./src\n",
                        "      m55_he:\n        app: ./src\n      m55_hp:\n        app: ./hp\n")

V2M = """
    som:
      sku: E1M-V2M101
    preset: e1m-x-evk
    cores:
      a55_cluster:
        image: alp-image-edge
      m33_sm:
        app: ./cm33
    cameras:
""" + CAM

NL = chr(10)
A32_OFF = "      a32_cluster:" + NL + '        os: "off"' + NL
A55_IMAGE = "      a55_cluster:" + NL + "        image: alp-image-edge" + NL
A55_OFF = "      a55_cluster:" + NL + '        os: "off"' + NL
HE_APP = "      m55_he:" + NL + "        app: ./src" + NL
HE_OFF = "      m55_he:" + NL + '        os: "off"' + NL

SHIELD = "e1m_evk_rpi_csi innomaker_cam_ov9281"
OV9281 = "innomaker_cam_ov9281"


def _write(tmp_path: Path, body: str, module: str = OV9281, core: str = "",
           extra: str = "") -> Path:
    return _write_board(tmp_path, body.format(
        module=module, extra=extra,
        core=f", core: {core}" if core else ""))


def _plan(path: Path) -> dict:
    return json.loads(emit_build_plan(
        load_board_yaml(path), board_yaml=path, build_root=Path("build")))


def _slice(plan: dict, core: str) -> dict:
    return next(s for s in plan["slices"] if s["coreId"] == core)


def _args(plan: dict, core: str) -> list[str]:
    return (_slice(plan, core)["command"] or {"args": []})["args"]


def _artefact(slice_: dict, name: str) -> dict:
    return next(c for c in slice_["configArtefacts"]
                if c["path"].endswith("/" + name))


def _b003(path: Path) -> list[str]:
    return [d.message for d in validate_board_yaml(path)
            if d.code == "ALP-B003" and "camera" in d.message]


def _blocked(plan: dict) -> set[str]:
    return {w["coreId"] for w in plan["warnings"]
            if w["code"] == "camera-select-failed"}


def test_aen_zephyr_shield_define_and_cmake_args(tmp_path: Path) -> None:
    plan = _plan(_write(tmp_path, AEN))
    he = _slice(plan, "m55_he")
    assert f"-DSHIELD={SHIELD}" in he["command"]["args"]
    assert not plan["warnings"]
    # the stock-shim HP core is not a camera owner
    assert not any(a.startswith("-DSHIELD") for a in _args(plan, "m55_hp"))
    assert f"-DSHIELD={SHIELD}" in _artefact(he, "cmake-args.txt")["contents"]


def test_explicit_core_matches_implied_owner(tmp_path: Path) -> None:
    plan = _plan(_write(tmp_path, AEN, core="m55_he"))
    assert f"-DSHIELD={SHIELD}" in _args(plan, "m55_he")


def test_aen_module_without_zephyr_shield_blocks_and_validates(tmp_path: Path) -> None:
    path = _write(tmp_path, AEN, module="raspberry_pi_camera_module_2")
    plan = _plan(path)
    assert _slice(plan, "m55_he")["command"] is None
    w = next(w for w in plan["warnings"] if w["code"] == "camera-select-failed")
    assert "zephyr_shield" in w["message"]
    assert any("zephyr_shield" in m for m in _b003(path))


def test_missing_board_overlay_blocks_and_validates(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setattr(camera_owner, "has_overlay", lambda *a: False)
    path = _write(tmp_path, AEN)
    assert "m55_he" in _blocked(_plan(path))
    assert any("no Zephyr shield overlay" in m for m in _b003(path))


def test_connector_without_zephyr_shields_is_error_for_zephyr_owner(
        tmp_path: Path) -> None:
    # X-EVK CAM0 declares no zephyr_shields: a Zephyr CM33 app cannot own it.
    path = _write(tmp_path, V2M, core="m33_sm")
    assert "m33_sm" in _blocked(_plan(path))
    assert any("zephyr_shields" in m for m in _b003(path))


def test_v2m_linux_camera_with_cm33_app_builds_both(tmp_path: Path) -> None:
    path = _write(tmp_path, V2M, module="raspberry_pi_global_shutter_camera",
                  core="a55_cluster")
    plan = _plan(path)
    assert not plan["warnings"] and not _b003(path)
    conf = _artefact(_slice(plan, "a55_cluster"), "local.conf")["contents"]
    assert 'ALP_CAMERA_CAM0 = "raspberry_pi_global_shutter_camera"' in conf
    # the CM33 customer app still builds, with no camera shield
    cm33 = _args(plan, "m33_sm")
    assert cm33 and not any("SHIELD" in a for a in cm33)


def test_v2m_cm33_app_and_linux_camera_without_core_a55_owns_it(
        tmp_path: Path) -> None:
    # X-EVK CAM0 has `linux: true` and no zephyr_shields: only the A55 can own it.
    path = _write(tmp_path, V2M, module="raspberry_pi_global_shutter_camera")
    plan = _plan(path)
    assert not _b003(path) and not plan["warnings"]
    conf = _artefact(_slice(plan, "a55_cluster"), "local.conf")["contents"]
    assert 'ALP_CAMERA_CAM0 = "raspberry_pi_global_shutter_camera"' in conf
    assert _args(plan, "m33_sm") and not any("SHIELD" in a for a in _args(plan, "m33_sm"))


def test_aen_default_topology_a32_yocto_does_not_make_it_ambiguous(
        tmp_path: Path) -> None:
    # The E1M-EVK connector is Zephyr-only: the A32 Yocto core is not a candidate.
    path = _write(tmp_path, AEN.replace(A32_OFF, ""))
    plan = _plan(path)
    assert not _b003(path)
    assert f"-DSHIELD={SHIELD}" in _args(plan, "m55_he")


def test_zero_candidates_message_names_the_supported_os(tmp_path: Path) -> None:
    # V2M, CM33 app only (A55 parked): the connector supports Linux, not Zephyr.
    body = V2M.replace(A55_IMAGE, A55_OFF)
    path = _write(tmp_path, body, module="raspberry_pi_global_shutter_camera")
    assert any("supports Linux (Yocto) only" in m for m in _b003(path))
    # AEN, connector Zephyr-only, Zephyr core parked.
    body = AEN.replace(HE_APP, HE_OFF)
    assert any("supports Zephyr only" in m for m in _b003(_write(tmp_path, body)))


def test_explicit_yocto_core_needs_connector_linux_flag(tmp_path: Path) -> None:
    path = _write(tmp_path, AEN.replace(A32_OFF, ""), core="a32_cluster")
    assert any("does not declare Linux support" in m for m in _b003(path))
    assert "a32_cluster" in _blocked(_plan(path))


def test_v2m_single_linux_owner_has_yocto_line(tmp_path: Path) -> None:
    body = V2M.replace("      m33_sm:\n        app: ./cm33\n", "")
    plan = _plan(_write(tmp_path, body, module="raspberry_pi_global_shutter_camera"))
    conf = _artefact(_slice(plan, "a55_cluster"), "local.conf")["contents"]
    assert 'ALP_CAMERA_CAM0 = "raspberry_pi_global_shutter_camera"' in conf
    assert not plan["warnings"]


def test_he_and_hp_apps_are_ambiguous_until_core_is_set(
        tmp_path: Path, monkeypatch) -> None:
    path = _write(tmp_path, AEN_HE_HP)
    assert any("ambiguous camera owner" in m for m in _b003(path))
    assert _blocked(_plan(path)) == {"m55_he", "m55_hp"}

    # HP has no J5 board overlay in the tree; ownership is what is under test.
    monkeypatch.setattr(camera_owner, "has_overlay", lambda *a: True)
    path = _write(tmp_path, AEN_HE_HP, core="m55_hp")
    plan = _plan(path)
    assert not _b003(path) and not plan["warnings"]
    assert f"-DSHIELD={SHIELD}" in _args(plan, "m55_hp")
    assert not any("SHIELD" in a for a in _args(plan, "m55_he"))


def test_core_must_be_a_candidate(tmp_path: Path) -> None:
    # m55_hp runs the stock shim: not a candidate.
    path = _write(tmp_path, AEN, core="m55_hp")
    assert any("'m55_hp' is not a Zephyr core" in m for m in _b003(path))
    path = _write(tmp_path, AEN, core="nope")
    assert any("'nope'" in m for m in _b003(path))


def test_no_candidate_core_is_error(tmp_path: Path) -> None:
    body = AEN.replace("      m55_he:\n        app: ./src\n",
                       "      m55_he:\n        os: \"off\"\n")
    assert any("no core can own the camera" in m
               for m in _b003(_write(tmp_path, body)))


def test_two_cameras_with_the_same_module_shield_rejected(tmp_path: Path) -> None:
    plans = camera_owner.plan_cameras(
        [{"connector": "CAM0", "module": "m"}, {"connector": "CAM1", "module": "m"}],
        {c: {"zephyr_shields": ["carrier"]} for c in ("CAM0", "CAM1")},
        {"c": {"os": "zephyr", "app": "./a", "board": None}},
        {"m": {"zephyr_shield": "modshield"}}, REPO)
    assert not plans[0].errors
    assert "single instance" in plans[1].errors[0]


BOOT = """
    boot:
      method: mcuboot
      signing:
        algorithm: ecdsa_p256
        key_file: keys/mcuboot_shared_dev_ecdsa_p256.pem
"""


def test_sysbuild_scopes_shield_to_the_app_image(tmp_path: Path) -> None:
    app = tmp_path / "src"
    app.mkdir()
    (tmp_path / "CMakeLists.txt").write_text("", encoding="utf-8")
    path = _write(tmp_path, AEN + "", extra="")
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    doc.update(yaml.safe_load(BOOT))
    path.write_text(yaml.safe_dump(doc), encoding="utf-8")
    args = _args(_plan(path), "m55_he")
    assert "--sysbuild" in args
    image = tmp_path.name
    assert f"-D{image}_SHIELD={SHIELD}" in args
    assert not any(a.startswith("-DSHIELD=") for a in args)


def test_inline_board_camera_connectors(tmp_path: Path) -> None:
    doc = {
        "name": "mine", "som": {"sku": "E1M-AEN803"},
        "cores": {"a32_cluster": {"os": "off"}, "m55_he": {"app": "./src"}},
        "populated": {},
        "e1m_routes": {"buses": [{"e1m": "E1M_I2C1", "macro": "MY_I2C"}]},
        "camera_connectors": {"CAM0": {
            "refdes": "J1", "csi": "E1M_CSI0", "lanes": 2, "i2c": "MY_I2C",
            "zephyr_shields": ["e1m_evk_rpi_csi"]}},
        "cameras": [{"connector": "CAM0", "module": OV9281}],
    }
    path = tmp_path / "board.yaml"
    path.write_text(yaml.safe_dump(doc), encoding="utf-8")
    assert not _b003(path)
    assert f"-DSHIELD={SHIELD}" in _args(_plan(path), "m55_he")
    doc["camera_connectors"]["CAM0"].pop("zephyr_shields")
    path.write_text(yaml.safe_dump(doc), encoding="utf-8")
    assert any("zephyr_shields" in m for m in _b003(path))
    assert "m55_he" in _blocked(_plan(path))


@pytest.mark.parametrize("sku", sorted(
    p.stem for p in (REPO / "metadata" / "e1m_modules").glob("E1M-*.yaml")))
def test_validator_core_resolution_matches_loader(tmp_path: Path, sku: str) -> None:
    """The validator resolves core os/app from raw YAML; the planner from the
    loader.  They must agree on every shipped SoM's default topology."""
    som = yaml.safe_load((next((REPO / "metadata" / "e1m_modules").rglob(
        f"{sku}.yaml"))).read_text(encoding="utf-8"))
    cores_yaml = "".join("  " + c + ": {}" + chr(10) for c in som["topology"])
    path = _write_board(
        tmp_path, "som:" + chr(10) + "  sku: " + sku + chr(10) + "cores:" + chr(10)
        + cores_yaml)
    project = load_board_yaml(path)
    resolved = camera_owner.resolve_cores({}, som.get("topology"))
    assert {c: v["os"] for c, v in resolved.items()} == {
        c: s.os for c, s in project.cores.items()}
    assert (camera_owner.candidates(resolved)
            == camera_owner.candidates({c: {"os": s.os, "app": s.app}
                                        for c, s in project.cores.items()}))
