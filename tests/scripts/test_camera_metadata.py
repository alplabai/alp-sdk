"""Camera metadata (#2615): chip-v1 `mipi`/`drivers`, camera-module-v1, the
carrier `camera_connectors`, the SoC `linux_dt` map, and the project
board.yaml `cameras:` cross-checks."""

import json
import sys
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import validate_metadata  # noqa: E402
from alp_cli.validator import validate_board_yaml  # noqa: E402

META = REPO / "metadata"


def _validator(name):
    schema = json.loads((META / "schemas" / name).read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def _errors(validator, doc):
    return [e.message for e in validator.iter_errors(doc)]


def _load(path):
    return yaml.safe_load(path.read_text(encoding="utf-8"))


# --- chip-v1 -----------------------------------------------------------

def test_ov9281_drivers_block():
    chip = _load(META / "chips" / "ov9281.yaml")
    assert chip["mipi"] == {"lanes_supported": [1, 2], "role": "csi2_tx"}
    assert chip["drivers"]["zephyr"]["compatible"] == "ovti,ov9281"
    assert chip["drivers"]["linux"] == {
        "compatible": "ovti,ov9282",
        "kconfig": "CONFIG_VIDEO_OV9282",
        "link_freqs_hz": [400000000],
    }


@pytest.mark.parametrize("patch", [
    {"mipi": {"lanes_supported": [2], "role": "bogus"}},
    {"mipi": {"lanes_supported": []}},
    {"mipi": {"role": "csi2_tx"}},
    {"drivers": {"linux": {"kconfig": "VIDEO_OV9282"}}},
    {"drivers": {"zephyr": {"compatible": "ov9281"}}},
])
def test_chip_schema_rejects_bad_mipi_and_drivers(patch):
    chip = {**_load(META / "chips" / "ov9281.yaml"), **patch}
    assert _errors(_validator("chip-v1.schema.json"), chip)


# --- camera-module-v1 --------------------------------------------------

MODULES = sorted((META / "camera_modules").glob("*.yaml"))


def test_every_shield_has_a_module():
    shields = {p.name for p in (REPO / "zephyr/boards/shields").iterdir()}
    fitted = {_load(m).get("zephyr_shield") for m in MODULES}
    for sensor in ("innomaker_cam_ov9281", "innomaker_cam_imx335",
                   "raspberry_pi_camera_module_1",
                   "raspberry_pi_global_shutter_camera"):
        assert sensor in shields and sensor in fitted


@pytest.mark.parametrize("path", MODULES, ids=lambda p: p.stem)
def test_modules_valid(path):
    v = _validator("camera-module-v1.schema.json")
    assert not _errors(v, _load(path))
    assert not validate_metadata._check_camera_module_semantics([path])


def test_module_matches_shield_overlay():
    m = _load(META / "camera_modules" / "innomaker_cam_ov9281.yaml")
    assert (m["chip"], m["i2c_addr_7bit"], m["xclk_hz"], m["lanes"]) == (
        "ov9281", 0x60, 24000000, 2)


def test_module_schema_rejects_missing_and_extra():
    v = _validator("camera-module-v1.schema.json")
    good = _load(META / "camera_modules" / "innomaker_cam_ov9281.yaml")
    assert _errors(v, {k: x for k, x in good.items() if k != "xclk_hz"})
    assert _errors(v, {**good, "bogus": 1})
    assert _errors(v, {**good, "lanes": 8})


def test_module_semantics_reject_unknown_chip_and_id_mismatch(tmp_path):
    bad = tmp_path / "wrong_name.yaml"
    bad.write_text(yaml.safe_dump({
        "schema_version": 1, "module_id": "other", "display_name": "x",
        "chip": "no_such_chip", "i2c_addr_7bit": 1, "xclk_hz": 1, "lanes": 1}), encoding="utf-8")
    msgs = [m for _, ms in validate_metadata._check_camera_module_semantics([bad])
            for m in ms]
    assert len(msgs) == 2


# --- carrier + SoM + SoC -------------------------------------------------

def test_carrier_cam0_valid_and_references_resolve():
    path = META / "boards" / "e1m-x-evk.yaml"
    assert not _errors(_validator("board-preset.schema.json"), _load(path))
    assert _load(path)["camera_connectors"]["CAM0"]["refdes"] == "J5"
    assert not validate_metadata._check_board_camera_connectors([path])


def test_carrier_connector_with_unknown_macro_rejected(tmp_path):
    doc = _load(META / "boards" / "e1m-x-evk.yaml")
    doc["camera_connectors"]["CAM0"]["enable"] = "XEVK_PIN_NOPE"
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump(doc), encoding="utf-8")
    assert validate_metadata._check_board_camera_connectors([p])


@pytest.mark.parametrize("sku", [f"E1M-{f}{n}" for f in ("V2N", "V2M")
                                 for n in (101, 102, 103)])
def test_som_camera_routes(sku):
    som = _load(META / "e1m_modules" / f"{sku}.yaml")
    assert not _errors(_validator("som-preset-v2.schema.json"), som)
    routes = {r["e1m"]: r for r in som["pad_routes"]}
    assert routes["E1M_X_I2C2"]["dispatch_pin"] == "RIIC2"
    assert routes["E1M_X_CSI0"]["dispatch_pin"] == "CSI0"
    assert routes["E1M_X_GPIO_IO18"]["dispatch"] == "TBD"


def test_soc_linux_dt():
    soc = json.loads((META / "socs/renesas/rzv2n/n44.json").read_text(encoding="utf-8"))
    assert not _errors(_validator("soc-spec-v1.schema.json"), soc)
    assert soc["linux_dt"]["RIIC2"] == {"label": "i2c2", "pinmux": {"P34": 1, "P35": 1}}
    assert soc["linux_dt"]["CSI0"] == {"label": "csi20", "capture": "cru0"}


# --- project board.yaml `cameras:` ---------------------------------------

def _project(tmp_path, cameras):
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump({
        "som": {"sku": "E1M-V2M103"}, "preset": "e1m-x-evk",
        "cores": {"a55_cluster": {"os": "linux", "app": "./src"}},
        "cameras": cameras}), encoding="utf-8")
    return p


def test_cameras_valid(tmp_path):
    p = _project(tmp_path, [{"connector": "CAM0", "module": "innomaker_cam_ov9281"}])
    c = validate_board_yaml(p)
    assert not [d for d in c if "camera" in d.message], [d.message for d in c]


@pytest.mark.parametrize("entry,needle", [
    ({"connector": "CAM0", "module": "no_such_module"}, "unknown camera module"),
    ({"connector": "CAM7", "module": "innomaker_cam_ov9281"}, "not a camera connector"),
])
def test_cameras_unknown_module_or_connector_is_error(tmp_path, entry, needle):
    c = validate_board_yaml(_project(tmp_path, [entry]))
    assert c.has_errors()
    assert any(needle in d.message for d in c)


def test_cameras_schema_rejects_extra_key(tmp_path):
    c = validate_board_yaml(_project(
        tmp_path, [{"connector": "CAM0", "module": "innomaker_cam_ov9281", "x": 1}]))
    assert c.has_errors()
