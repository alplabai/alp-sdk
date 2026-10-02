# SPDX-License-Identifier: Apache-2.0
"""scripts/gen_camera_dt.py (#2633): committed outputs are in sync, the OV9281
fragment keeps the bench-proven property set, a NEW camera module needs
metadata only, and a missing route is a named gap, never a guess."""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_camera_dt as gen  # noqa: E402

_COPY = ("metadata/boards", "metadata/camera_modules", "metadata/chips",
         "metadata/e1m_modules", "metadata/socs/renesas", "metadata/os",
         "meta-alp-sdk/recipes-kernel/linux/linux-renesas")
OV9281 = gen.LINUX_DIR / "e1m-x-evk-cam0-innomaker_cam_ov9281.dtsi"


@pytest.fixture(scope="module")
def _base(tmp_path_factory):
    root = tmp_path_factory.mktemp("camtree")
    for d in _COPY:
        shutil.copytree(REPO / d, root / d)
    (root / "docs").mkdir()
    shutil.copy(REPO / gen.DOC, root / gen.DOC)
    return root


@pytest.fixture
def tree(_base, tmp_path):
    root = tmp_path / "t"
    shutil.copytree(_base, root)
    return root


def _edit_yaml(path: Path, fn) -> None:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    fn(doc)
    path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")


def parse_dt(text: str) -> dict[str, dict[str, str]]:
    """Flat {node path: {property: value}} of a dtsi (comments dropped)."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    out: dict[str, dict[str, str]] = {}
    stack: list[str] = []

    def path() -> str:
        return "/".join(stack).replace("//", "/")

    for m in re.finditer(r"([^{};]*)\{|\}\s*;|([^{};]+);", text):
        if m.group(0).startswith("}"):
            stack.pop()
        elif m.group(1) is not None:
            name = m.group(1).strip()
            name = name.split(":")[-1].strip() if ":" in name else name
            stack.append(name)
            out.setdefault(path(), {})
        else:
            k, _, v = m.group(2).strip().partition("=")
            out[path()][k.strip()] = " ".join(v.split()) or "true"
    return out


# The property set of the bench-proven hand-written OV9281 fragment
# (e1m-x-evk-cam0-ov9281.dtsi before it became generated) plus the alias.
OV9281_EXPECTED = {
    "/aliases": {"alp-camera0": "&cam0_sensor"},
    "/cam0-xclk": {"compatible": '"fixed-clock"', "#clock-cells": "<0>",
                   "clock-frequency": "<24000000>"},
    "&gd32_gpio/cam0-mux-sel-hog": {"gpio-hog": "true", "gpios": "<7 GPIO_ACTIVE_HIGH>",
                                    "output-low": "true", "line-name": '"cam0-mux-sel"'},
    "&pinctrl/i2c2": {"pinmux": "<RZV2N_PORT_PINMUX(3, 4, 1)>, <RZV2N_PORT_PINMUX(3, 5, 1)>"},
    "&i2c2": {"pinctrl-0": "<&i2c2_pins>", "pinctrl-names": '"default"',
              "clock-frequency": "<400000>", "status": '"okay"'},
    "&i2c2/camera@60": {"compatible": '"ovti,ov9281"', "reg": "<0x60>", "clocks": "<&cam0_xclk>"},
    "&i2c2/camera@60/port/endpoint": {
        "remote-endpoint": "<&cam0_csi_in>", "data-lanes": "<1 2>",
        "link-frequencies": "/bits/ 64 <400000000>", "clock-noncontinuous": "true"},
    "&csi20": {"status": '"okay"'},
    "&csi20/ports": {"#address-cells": "<1>", "#size-cells": "<0>"},
    "&csi20/ports/port@0": {"reg": "<0>"},
    "&csi20/ports/port@0/endpoint": {
        "remote-endpoint": "<&cam0_sensor_out>", "data-lanes": "<1 2>",
        "lane-polarities": "<1 1 1>"},
    "&cru0": {"status": '"okay"'},
}


def _nonempty(d):
    return {k: v for k, v in d.items() if v}


def test_committed_outputs_are_in_sync():
    assert gen.main(["--check"]) == 0


def test_ov9281_fragment_keeps_the_hand_written_property_set():
    got = parse_dt((REPO / OV9281).read_text(encoding="utf-8"))
    assert _nonempty(got) == _nonempty(OV9281_EXPECTED)


def test_new_module_needs_metadata_only(tree):
    """Add a chip + a module in the tmp tree; no code changes; a fragment, a
    kconfig line and a docs row appear."""
    chip = yaml.safe_load((tree / "metadata/chips/ov9281.yaml").read_text(encoding="utf-8"))
    chip.update(chip_id="newsensor")
    chip["i2c"] = {"addresses": [{"addr_7bit": 0x3c}], "max_clock_hz": 100000}
    chip["drivers"]["linux"] = {
        "compatible": "acme,newsensor", "kconfig": "CONFIG_VIDEO_NEWSENSOR",
        "link_freqs": [{"lanes": 1, "link_freqs_hz": [123000000, 456000000]}],
        "clock_name": "xvclk", "reset_property": "reset-gpios"}
    (tree / "metadata/chips/newsensor.yaml").write_text(yaml.safe_dump(chip), encoding="utf-8")
    (tree / "metadata/camera_modules/acme_cam.yaml").write_text(yaml.safe_dump({
        "schema_version": 1, "module_id": "acme_cam", "display_name": "Acme Cam (NEWSENSOR)",
        "chip": "newsensor", "i2c_addr_7bit": 0x3c, "xclk_hz": 27000000, "lanes": 1}),
        encoding="utf-8")
    out, problems = gen.generate(tree)
    assert problems == []
    frag = out[gen.LINUX_DIR / "e1m-x-evk-cam0-acme_cam.dtsi"]
    dt = parse_dt(frag)
    assert dt["&i2c2/camera@3c"]["compatible"] == '"acme,newsensor"'
    assert dt["&i2c2/camera@3c"]["clock-names"] == '"xvclk"'
    assert dt["&i2c2"]["clock-frequency"] == "<100000>"
    assert dt["&i2c2/camera@3c/port/endpoint"]["data-lanes"] == "<1>"
    assert dt["&i2c2/camera@3c/port/endpoint"]["link-frequencies"] == "/bits/ 64 <123000000> <456000000>"
    assert dt["&csi20/ports/port@0/endpoint"]["lane-polarities"] == "<1 1>"
    assert dt["/cam0-xclk"]["clock-frequency"] == "<27000000>"
    assert "CONFIG_VIDEO_NEWSENSOR=y" in out[gen.LINUX_DIR / gen.CFG_NAME]
    assert "| Acme Cam (NEWSENSOR) |" in out[gen.DOC]


def test_unrouted_enable_and_reset_are_notes_not_gaps(tree):
    out, problems = gen.generate(tree)
    assert problems == []
    frag = out[OV9281]
    assert "XEVK_PIN_CAM0_EN: not routed" in frag and "reset-gpios" not in frag
    assert "GAP" not in frag


def test_tbd_route_is_a_named_gap_and_fails_check(tree, capsys):
    for sku in ("E1M-V2N101", "E1M-V2M101"):
        def tbd(doc):
            for r in doc["pad_routes"]:
                if r["e1m"] == "E1M_X_GPIO_IO16":
                    r.clear()
                    r.update(e1m="E1M_X_GPIO_IO16", dispatch="TBD")
        _edit_yaml(tree / f"metadata/e1m_modules/{sku}.yaml", tbd)
    out, problems = gen.generate(tree)
    assert any("metadata gap" in p and "E1M_X_GPIO_IO16" in p and "TBD" in p for p in problems)
    assert "GAP" in out[OV9281] and "cam0-mux-sel-hog" not in out[OV9281]
    assert gen.main(["--check"], root=tree) == 1
    assert "E1M_X_GPIO_IO16" in capsys.readouterr().err


def test_soms_that_resolve_differently_are_rejected(tree):
    def drop_csi(doc):
        doc["pad_routes"] = [r for r in doc["pad_routes"] if r["e1m"] != "E1M_X_CSI0"]
    _edit_yaml(tree / "metadata/e1m_modules/E1M-V2M101.yaml", drop_csi)
    _, problems = gen.generate(tree)
    assert any("SoMs resolve to different routes" in p for p in problems)


def test_module_wider_than_the_connector_gets_no_fragment(tree):
    _edit_yaml(tree / "metadata/camera_modules/innomaker_cam_ov9281.yaml",
               lambda d: d.update(lanes=4))
    out, _ = gen.generate(tree)
    assert OV9281 not in out


def test_removed_module_deletes_its_generated_fragment(tree):
    assert gen.main([], root=tree) == 0
    (tree / "metadata/camera_modules/raspberry_pi_camera_module_2.yaml").unlink()
    stale = tree / gen.LINUX_DIR / "e1m-x-evk-cam0-raspberry_pi_camera_module_2.dtsi"
    assert stale.is_file()
    assert gen.main(["--check"], root=tree) == 1
    assert gen.main([], root=tree) == 0
    assert not stale.exists()


def test_gpio_line_must_match_the_som_dtsi_names(tree):
    p = tree / gen.LINUX_DIR / "e1m-v2n-som.dtsi"
    p.write_text(p.read_text(encoding="utf-8").replace('"IO14", "IO16"', '"IO16", "IO14"'),
                 encoding="utf-8")
    _, problems = gen.generate(tree)
    assert any("gpio-line-names" in x and "IO16" in x for x in problems)


def test_slice_local_conf_emits_the_camera_knob_only_with_cameras(tmp_path):
    from _orchestrate_support import _write_board
    from alp_orchestrate import _slice_local_conf, load_board_yaml
    body = """
som:
  sku: E1M-V2N101
preset: e1m-x-evk
cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
"""
    def conf(extra):
        project = load_board_yaml(_write_board(tmp_path, body + extra))
        return _slice_local_conf(project, project.cores["a55_cluster"])
    without = conf("")
    with_cam = conf("cameras:\n  - { connector: CAM0, module: innomaker_cam_ov9281 }\n")
    assert "ALP_CAMERA" not in without
    assert 'ALP_CAMERA_CAM0 = "innomaker_cam_ov9281"' in with_cam
    assert with_cam.replace('# Cameras (board.yaml `cameras:` block)\n'
                            'ALP_CAMERA_CAM0 = "innomaker_cam_ov9281"\n', "") == without
