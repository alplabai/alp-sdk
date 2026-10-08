"""Camera metadata (#2615): chip-v1 `mipi`/`drivers`, camera-module-v1, the
carrier `camera_connectors`, the SoC `linux_dt` map, and the project
board.yaml `cameras:` cross-checks."""

import json
import re
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
        "link_freqs": [{"lanes": 2, "link_freqs_hz": [400000000]}],
        "supplies": ["avdd", "dovdd", "dvdd"],
        "reset_property": "reset-gpios",
        "xclk_supported_hz": [24000000],
        "endpoint_flags": ["clock-noncontinuous"],
    }


@pytest.mark.parametrize("patch", [
    {"mipi": {"lanes_supported": [2], "role": "bogus"}},
    {"mipi": {"lanes_supported": []}},
    {"mipi": {"role": "csi2_tx"}},
    {"drivers": {"linux": {"kconfig": "VIDEO_OV9282"}}},
    {"drivers": {"zephyr": {"compatible": "ov9281"}}},
    {"drivers": {"linux": {"compatible": "ovti,ov9282",
                           "link_freqs": [{"lanes": 2}]}}},
    {"drivers": {"linux": {"compatible": "ovti,ov9282", "reset_property": "gpio"}}},
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


# --- Zephyr shield overlay parity -----------------------------------------

SHIELDS = REPO / "zephyr" / "boards" / "shields"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _block(text, header):
    """Body of the first `<header> { ... };` (brace matched), or None."""
    m = re.search(re.escape(header) + r"\s*\{", text)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def _props(body):
    """Direct `name = <...>;` / `name = "str";` / bare `name;` properties of a
    node body (child nodes are blanked out first)."""
    flat, depth = [], 0
    for ch in body:
        if ch == "{":
            depth += 1
        flat.append(ch if depth == 0 else " ")
        if ch == "}":
            depth -= 1
    out = {}
    for stmt in "".join(flat).split(";"):
        stmt = stmt.strip()
        if not stmt or "{" in stmt:
            continue
        name, _, val = stmt.partition("=")
        val = val.strip()
        if not val:
            out[name.strip()] = True
        elif val.startswith('"'):
            out[name.strip()] = val.strip('"')
        elif val.startswith("<"):
            out[name.strip()] = [int(x, 0) for x in
                                 re.findall(r"0x[0-9a-fA-F]+|\d+", val)]
    return out


def _sensor_shields():
    """Every shield overlay that selects a camera (`zephyr,camera`): found by
    scanning, not from a list, so a new sensor shield is covered at once."""
    found = {}
    for ov in sorted(SHIELDS.glob("*/*.overlay")):
        text = _strip_comments(ov.read_text(encoding="utf-8"))
        if "zephyr,camera" in text:
            found[ov.parent.name] = text
    return found


def _parse_overlay(text):
    i2c = _block(text, "&csi_i2c")
    node = re.search(r"\w+:\s*\w+@[0-9a-f]+", i2c).group(0)
    sensor = _block(i2c, node)
    clk = _block(text, re.search(r"\w+:\s*[\w-]+(?=\s*\{\s*compatible\s*=\s*\"fixed-clock\")",
                                 text).group(0))
    ep = _props(_block(sensor, "endpoint"))
    return {
        "compatible": _props(sensor)["compatible"],
        "reg": _props(sensor)["reg"][0],
        "clock": _props(clk)["clock-frequency"][0],
        "lanes": len(ep["data-lanes"]),
        "endpoint": ep,
        "hosts": {"snps,designware-csi": _props(_block(text, "&csi") or ""),
                  "vsi,isp-pico": _props(_block(text, "&isp") or "")},
    }


def test_sensor_shields_are_discovered():
    assert len(_sensor_shields()) >= 4


@pytest.mark.parametrize("shield", sorted(_sensor_shields()))
def test_module_matches_shield_overlay(shield):
    ov = _parse_overlay(_sensor_shields()[shield])
    mods = [m for m in map(_load, MODULES) if m.get("zephyr_shield") == shield]
    assert len(mods) == 1, f"shield {shield} needs exactly one camera module"
    m = mods[0]
    chip = _load(META / "chips" / f"{m['chip']}.yaml")
    assert ov["compatible"] == chip["drivers"]["zephyr"]["compatible"]
    assert ov["reg"] == m["i2c_addr_7bit"]
    assert ov["clock"] == m["xclk_hz"]
    assert ov["lanes"] == m["lanes"]
    # Sensor-side endpoint booleans are chip facts.
    flags = {k for k, v in ov["endpoint"].items() if v is True}
    assert flags == set(chip["drivers"]["zephyr"].get("endpoint_flags", []))
    # Every property the overlay sets on the shared host nodes is carried by
    # the module, so a generator loses nothing.
    declared = m.get("zephyr_host_overrides", {})
    for host, props in ov["hosts"].items():
        want = {k: (v[0] if isinstance(v, list) and len(v) == 1 else v)
                for k, v in props.items() if k != "status"}
        assert declared.get(host, {}) == want, (shield, host)


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
    assert soc["linux_dt"]["RIIC2"] == {"label": "i2c2", "pinmux": {"RIIC2_SDA2": 1, "RIIC2_SCL2": 1}}
    assert soc["linux_dt"]["CSI0"] == {"label": "csi20", "capture": "cru0",
                                         "kconfig": ["CONFIG_VIDEO_RZG2L_CSI2", "CONFIG_VIDEO_RZG2L_CRU"]}


# --- project board.yaml `cameras:` ---------------------------------------

def _project(tmp_path, cameras):
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump({
        "som": {"sku": "E1M-V2M103"}, "preset": "e1m-x-evk",
        "cores": {"a55_cluster": {"os": "yocto", "app": "./src"}},
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


def test_cameras_duplicate_connector_is_error(tmp_path):
    entry = {"connector": "CAM0", "module": "innomaker_cam_ov9281"}
    c = validate_board_yaml(_project(tmp_path, [entry, entry]))
    assert any("more than once" in d.message and d.code == "ALP-B003" for d in c)


def _inline_project(tmp_path, connector):
    routes = {"gpio": [{"e1m": "E1M_X_GPIO_IO1", "macro": "MY_EN"}],
              "buses": [{"e1m": "E1M_X_I2C2", "macro": "MY_I2C"}]}
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump({
        "name": "mine", "som": {"sku": "E1M-V2M103"},
        "cores": {"a55_cluster": {"os": "yocto", "app": "./src"}},
        "populated": {}, "e1m_routes": routes,
        "camera_connectors": {"CAM0": {
            "refdes": "J1", "csi": "E1M_X_CSI0", "lanes": 2, "i2c": "MY_I2C",
            "enable": "MY_EN", **connector}}}), encoding="utf-8")
    return p


def test_inline_connector_ok(tmp_path):
    c = validate_board_yaml(_inline_project(tmp_path, {"lane_polarity": [0, 0, 0]}))
    assert not [d for d in c if d.code == "ALP-B003"], [d.message for d in c]


@pytest.mark.parametrize("bad,needle", [
    ({"enable": "NOPE"}, "not a macro"),
    ({"lane_polarity": [1, 1]}, "expected 3"),
])
def test_inline_connector_bad_macro_or_polarity(tmp_path, bad, needle):
    c = validate_board_yaml(_inline_project(tmp_path, bad))
    assert any(needle in d.message and d.code == "ALP-B003" for d in c)


def test_preset_connector_lane_polarity_length_checked(tmp_path):
    doc = _load(META / "boards" / "e1m-x-evk.yaml")
    doc["camera_connectors"]["CAM0"]["lane_polarity"] = [1, 1]
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump(doc), encoding="utf-8")
    assert validate_metadata._check_board_camera_connectors([p])


def test_preset_and_inline_camera_connectors_are_exclusive(tmp_path):
    p = _project(tmp_path, [])
    doc = _load(p)
    doc["camera_connectors"] = {}
    p.write_text(yaml.safe_dump(doc), encoding="utf-8")
    assert validate_board_yaml(p).has_errors()


def test_camera_connector_def_identical_in_both_schemas():
    # JSON-Schema $ref does not cross files in this repo's validators, so the
    # connector definition is duplicated; this keeps the copies identical.
    def d(name):
        return json.loads((META / "schemas" / name).read_text(
            encoding="utf-8"))["$defs"]["camera_connector"]
    assert d("board.schema.json") == d("board-preset.schema.json")


def test_cam0_enable_is_active_low_and_supply_modelled():
    doc = _load(META / "boards" / "e1m-x-evk.yaml")
    route = next(r for r in doc["e1m_routes"]["gpio"]
                 if r["macro"] == doc["camera_connectors"]["CAM0"]["enable"])
    assert route["active_low"] is True
    assert doc["camera_connectors"]["CAM0"]["supply"] == "fixed-3v3"


def test_soc_linux_dt_pinmux_resolves_against_peripheral_map(tmp_path):
    soc = META / "socs/renesas/rzv2n/n44.json"
    assert not validate_metadata._check_soc_linux_dt([soc])
    bad = json.loads(soc.read_text(encoding="utf-8"))
    p = tmp_path / "bad.json"
    bad["linux_dt"]["RIIC2"]["pinmux"] = {"NOT_A_SIGNAL": 1}
    p.write_text(json.dumps(bad), encoding="utf-8")
    assert validate_metadata._check_soc_linux_dt([p])
    bad["linux_dt"]["RIIC2"] = {"label": "i2c99", "pinmux": {"RIIC2_SDA2": 1}}
    p.write_text(json.dumps(bad), encoding="utf-8")
    assert validate_metadata._check_soc_linux_dt([p])


def test_linux_kernel_drivers_file_valid():
    doc = _load(META / "os" / "linux-kernel-drivers.yaml")
    assert not _errors(_validator("linux-kernel-drivers-v1.schema.json"), doc)
    drivers = doc["kernels"]["6.1.141-cip43"]["drivers"]
    listed = {d["compatible"] for d in drivers}
    for chip in (META / "chips").glob("*.yaml"):
        lin = (_load(chip).get("drivers") or {}).get("linux")
        if lin:
            assert lin["compatible"] in listed, chip.stem


def test_linux_driver_facts_consistent_with_modules():
    for p in MODULES:
        m = _load(p)
        chip = _load(META / "chips" / f"{m['chip']}.yaml")
        lin = (chip.get("drivers") or {}).get("linux")
        if not lin:
            continue
        if "xclk_supported_hz" in lin:
            assert m["xclk_hz"] in lin["xclk_supported_hz"], p.stem
        if "link_freqs" in lin:
            assert m["lanes"] in {e["lanes"] for e in lin["link_freqs"]}, p.stem


def test_cameras_module_without_zephyr_shield_rejected_on_zephyr_core(tmp_path):
    p = tmp_path / "board.yaml"
    p.write_text(yaml.safe_dump({
        "som": {"sku": "E1M-AEN803"}, "preset": "e1m-evk",
        "cores": {"a32_cluster": {"os": "off"}, "m55_he": {"app": "./src"}},
        "cameras": [{"connector": "CAM0", "module": "raspberry_pi_camera_module_2"}]}),
        encoding="utf-8")
    c = validate_board_yaml(p)
    assert any("zephyr_shield" in d.message and d.code == "ALP-B003" for d in c)
    # same module on a Linux-only core is fine
    assert not [d for d in validate_board_yaml(_project(
        tmp_path, [{"connector": "CAM0", "module": "raspberry_pi_camera_module_2"}]))
        if "camera" in d.message]


def test_connector_zephyr_shields_must_exist(tmp_path):
    from alp_cli.validator import camera_connector_problems
    conn = {"CAM0": {"zephyr_shields": ["no_such_shield", "e1m_evk_rpi_csi"]}}
    msgs = camera_connector_problems(conn, {})
    assert len(msgs) == 1 and "no_such_shield" in msgs[0]


def test_linux_flag_only_on_cam0_of_rzv2n_boards(tmp_path):
    p = tmp_path / "board.yaml"

    def problems(doc):
        p.write_text(yaml.safe_dump(doc), encoding="utf-8")
        return validate_metadata._check_board_camera_connectors([p])

    ok = _load(META / "boards" / "e1m-x-evk.yaml")
    assert ok["camera_connectors"]["CAM0"]["linux"] is True
    assert not problems(ok)

    aen = _load(META / "boards" / "e1m-evk.yaml")
    aen["camera_connectors"]["CAM0"]["linux"] = True
    assert problems(aen)  # alif-ensemble: no Linux camera path

    cam1 = _load(META / "boards" / "e1m-x-evk.yaml")
    cam1["camera_connectors"]["CAM1"] = dict(cam1["camera_connectors"]["CAM0"])
    assert problems(cam1)  # only CAM0


def test_inline_linux_flag_follows_the_project_som_family(tmp_path):
    for sku, bad in (("E1M-V2M103", False), ("E1M-AEN803", True)):
        p = _inline_project(tmp_path, {"linux": True})
        doc = _load(p)
        doc["som"]["sku"] = sku
        doc["cores"] = {}
        p.write_text(yaml.safe_dump(doc), encoding="utf-8")
        hit = [d for d in validate_board_yaml(p)
               if d.code == "ALP-B003" and "linux" in d.message]
        assert bool(hit) is bad, (sku, [d.message for d in hit])
