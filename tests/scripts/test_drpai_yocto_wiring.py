"""Text-level pins for the DRP-AI access-control / DRP1 Yocto wiring."""

from pathlib import Path

L = Path(__file__).resolve().parents[2] / "meta-alp-sdk"


def _t(rel: str) -> str:
    return (L / rel).read_text(encoding="utf-8")


def test_0018_appended_and_gated_on_rz_drpai():
    s = _t("recipes-kernel/linux/linux-renesas_%.bbappend")
    line = next(x for x in s.splitlines() if "file://0018-" in x)
    assert line.startswith("SRC_URI:append") and "ALP_DRPAI_LAYER" in line


def test_drp1_stub_or_real_switch():
    s = _t("recipes-kernel/linux/linux-renesas_%.bbappend")
    assert 'if [ "${ALP_DRP1_DT_ENABLE}" = "1" ]' in s
    assert "rz-opencva" in s and "meta-rz-codecs" in s


def test_udev_package_rides_packageconfig_drpai():
    s = _t("recipes-core/alp-sdk/alp-sdk_0.6.bb")
    line = next(x for x in s.splitlines() if x.startswith("PACKAGECONFIG[drpai]"))
    assert "alp-drpai-udev" in line
    assert "@ALP_RUN_GROUP@" in _t("recipes-core/alp-sdk/files/alp-sdk-tmpfiles.conf")
