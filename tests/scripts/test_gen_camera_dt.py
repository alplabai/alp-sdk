# SPDX-License-Identifier: Apache-2.0
"""scripts/gen_camera_dt.py + scripts/check_camera_parity.py: the V2N/V2M Linux
camera DT and kernel config follow the camera metadata (#2633)."""
import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import check_camera_parity as gate  # noqa: E402
import gen_camera_dt as g  # noqa: E402

OUT = g.OUT_DIR.as_posix()
OV9281 = f"{OUT}/e1m-x-evk-cam0-innomaker_cam_ov9281.dtsi"
IMX219 = f"{OUT}/e1m-x-evk-cam0-raspberry_pi_camera_module_2.dtsi"
CFG = f"{OUT}/camera-sensors.cfg"


@pytest.fixture(scope="module")
def want():
    return g.generate(REPO)


@pytest.fixture
def tree(tmp_path):
    """A copy of just the files the generator reads, plus the committed output."""
    for sub in ("metadata/camera_modules", "metadata/chips", "metadata/boards", "metadata/socs/renesas/rzv2n",
                "metadata/e1m_modules/v2n", "metadata/os", OUT):
        shutil.copytree(REPO / sub, tmp_path / sub)
    for p in (REPO / "metadata/e1m_modules").glob("E1M-V2*.yaml"):
        shutil.copy(p, tmp_path / "metadata/e1m_modules" / p.name)
    return tmp_path


def test_ov9281_on_cam0(want):
    t = want[OV9281]
    assert 'compatible = "ovti,ov9282";' in t and "reg = <0x60>;" in t
    assert "clock-frequency = <24000000>;" in t
    # mux select: E1M IO16 is line 7 of the GD32 bridge gpio, driven low to pick J5
    assert "gpios = <7 GPIO_ACTIVE_HIGH>;\n\t\toutput-low;" in t
    assert "link-frequencies = /bits/ 64 <400000000>;" in t and "clock-noncontinuous;" in t
    assert "RZV2N_PORT_PINMUX(3, 4, 1)>, /* I2C2_SDA */" in t and "RZV2N_PORT_PINMUX(3, 5, 1)>; /* I2C2_SCL */" in t
    assert "lane-polarities = <1 1 1>;" in t
    for s in ("avdd", "dovdd", "dvdd"):
        assert f"{s}-supply = <&cam0_supply>;" in t
    # enable / reset have no SoM route yet: named, not driven
    assert "XEVK_PIN_CAM0_EN (E1M_X_GPIO_IO18): SoM route is TBD" in t
    assert "reset-gpios" not in t
    assert "&csi20 {" in t and "&cru0 {" in t


def test_imx219_on_cam0(want):
    t = want[IMX219]
    assert 'compatible = "sony,imx219";' in t and "reg = <0x10>;" in t
    assert "link-frequencies = /bits/ 64 <456000000>;" in t
    assert "clock-noncontinuous" not in t
    # supply names are spelled as the driver spells them (upper case)
    assert "VANA-supply = <&cam0_supply>;" in t


GS = f"{OUT}/e1m-x-evk-cam0-raspberry_pi_global_shutter_camera.dtsi"
IMX335 = f"{OUT}/e1m-x-evk-cam0-innomaker_cam_imx335.dtsi"
P0028 = "0028-media-i2c-add-imx296-backport.patch"
P0029 = "0029-media-i2c-imx335-2-lane-10-bit-binned-mode.patch"


def test_imx296_on_cam0_matches_the_bench_proven_dt(want):
    """IMX296LQ on E1M-V2M103 CAM0/J5, bench 2026-10-08 (60 fps, SBGGR10 1456x1088):
    1 lane, 54 MHz inck, RIIC2 400 kHz at 0x1a, lane-polarities <1 1>, the IO16 mux
    hog low, an always-on 3.3 V rail, no reset / enable GPIOs.  The explicit
    `sony,imx296lq` skips the SENSOR_INFO auto-identify."""
    t = want[GS]
    assert 'compatible = "sony,imx296lq";' in t and "reg = <0x1a>;" in t
    assert 'clock-names = "inck";' in t and "clock-frequency = <54000000>;" in t
    assert "clock-frequency = <400000>;" in t
    assert "data-lanes = <1>;" in t and "data-lanes = <1 2>" not in t
    assert "lane-polarities = <1 1>;" in t
    assert "gpios = <7 GPIO_ACTIVE_HIGH>;\n\t\toutput-low;" in t
    assert 'regulator-name = "cam0-3v3";' in t and "regulator-always-on;" in t
    for s in ("avdd", "dvdd", "ovdd"):
        assert f"{s}-supply = <&cam0_supply>;" in t
    assert "reset-gpios" not in t and "link-frequencies" not in t
    assert "CONFIG_VIDEO_IMX296=y" in want[CFG]


def test_every_fragment_aliases_its_sensor_as_alp_camera_n(want):
    """<alp/camera.h> camera_id N -> the `alp-camera<N>` alias -> the sensor node."""
    frags = [k for k in want if k.endswith(".dtsi")]
    assert frags
    for k in frags:
        assert "\taliases {\n\t\talp-camera0 = &cam0_sensor;\n\t};" in want[k], k


def test_imx335_two_lane_fragment_is_generated(want):
    assert "data-lanes = <1 2>;" in want[IMX335]
    assert "CONFIG_VIDEO_IMX335=y" in want[CFG]


def test_missing_patches_drop_fragment_and_kconfig(tree):
    """IMX296 is not in the 6.1 kernel and the native IMX335 is 4-lane only: without
    their alp patches there is no fragment (and no IMX296 config line)."""
    (tree / OUT / P0028).unlink()
    (tree / OUT / P0029).unlink()
    want = g.generate(tree)
    assert GS not in want and IMX335 not in want
    assert "IMX296" not in want[CFG]
    assert "CONFIG_VIDEO_IMX335=y" in want[CFG]  # native driver, so still built in
    assert OV9281 in want and IMX219 in want


def test_module_linux_compatible_defaults_to_the_chips(tree):
    p = tree / "metadata/camera_modules/raspberry_pi_global_shutter_camera.yaml"
    p.write_text(p.read_text(encoding="utf-8").replace('linux_compatible: "sony,imx296lq"\n', ""), encoding="utf-8")
    assert 'compatible = "sony,imx296";' in g.generate(tree)[GS]


def test_kernel_config_lists_receiver_and_available_sensors(want):
    assert set(want[CFG].splitlines()) >= {
        "CONFIG_VIDEO_RZG2L_CSI2=y", "CONFIG_VIDEO_RZG2L_CRU=y", "CONFIG_VIDEO_IMX219=y",
        "CONFIG_VIDEO_OV5647=y", "CONFIG_VIDEO_OV9282=y"}


def test_output_is_deterministic(want):
    assert g.generate(REPO) == want


def test_committed_files_are_in_sync():
    assert gate.find_problems(REPO) == []


def test_drift_is_reported(tree):
    assert gate.find_problems(tree) == []
    f = tree / OV9281
    f.write_text(f.read_text(encoding="utf-8") + "/* hand edit */\n", encoding="utf-8")
    assert gate.find_problems(tree) == [f"{OV9281} is stale or missing"]


def test_new_module_needs_regeneration_and_is_then_generated(tree):
    mod = yaml.safe_load((tree / "metadata/camera_modules/innomaker_cam_ov9281.yaml").read_text(encoding="utf-8"))
    mod.update(module_id="my_ov9281", i2c_addr_7bit=0x70)
    (tree / "metadata/camera_modules/my_ov9281.yaml").write_text(yaml.safe_dump(mod), encoding="utf-8")
    assert f"{OUT}/e1m-x-evk-cam0-my_ov9281.dtsi is stale or missing" in gate.find_problems(tree)
    for rel, text in g.generate(tree).items():
        (tree / rel).write_bytes(text.encode("utf-8"))
    assert gate.find_problems(tree) == []
    assert "reg = <0x70>;" in (tree / OUT / "e1m-x-evk-cam0-my_ov9281.dtsi").read_text(encoding="utf-8")


def test_orphan_fragment_is_reported(tree):
    (tree / OUT / "e1m-x-evk-cam0-gone.dtsi").write_text("x\n", encoding="utf-8")
    assert any("e1m-x-evk-cam0-gone.dtsi is generated for no" in m for m in gate.find_problems(tree))


def test_module_wider_than_the_connector_gets_no_fragment(tree):
    mod = yaml.safe_load((tree / "metadata/camera_modules/innomaker_cam_imx335.yaml").read_text(encoding="utf-8"))
    mod.update(module_id="wide", lanes=4)
    (tree / "metadata/camera_modules/wide.yaml").write_text(yaml.safe_dump(mod), encoding="utf-8")
    assert f"{OUT}/e1m-x-evk-cam0-wide.dtsi" not in g.generate(tree)


def test_chip_without_linux_driver_info_fails_loudly(tree):
    p = tree / "metadata/chips/ov5647.yaml"
    p.write_text(p.read_text(encoding="utf-8").replace("  linux:", "  linux_gone:"), encoding="utf-8")
    problems = gate.find_problems(tree)
    assert len(problems) == 1 and "cannot generate" in problems[0]


def test_disagreeing_som_routes_fail(tree):
    p = tree / "metadata/e1m_modules/E1M-V2N102.yaml"
    p.write_text(p.read_text(encoding="utf-8").replace(
        "{ e1m: E1M_X_I2C2, dispatch: direct, dispatch_pin: RIIC2 }",
        "{ e1m: E1M_X_I2C2, dispatch: direct, dispatch_pin: RIIC9 }"), encoding="utf-8")
    problems = gate.find_problems(tree)
    assert len(problems) == 1 and "pad_routes disagree" in problems[0]
