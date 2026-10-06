"""Text-level pins for the DRP-AI access-control / DRP1 Yocto wiring."""

from pathlib import Path

L = Path(__file__).resolve().parents[2] / "meta-alp-sdk"


def _t(rel: str) -> str:
    return (L / rel).read_text(encoding="utf-8")


def test_0018_appended_last_and_gated_on_rz_drpai():
    # Must be an anonymous-python append in the 6.1 bbappend: the wildcard
    # bbappend is evaluated before meta-rz-drpai's, so a SRC_URI:append there
    # would put 0018 ahead of the vendor drpai driver patches.
    s = _t("recipes-kernel/linux/linux-renesas_6.1.bbappend")
    assert "python ()" in s and "d.appendVar('SRC_URI'" in s
    assert "file://0018-" in s and "ALP_DRPAI_LAYER" in s
    assert "0018-" not in _t("recipes-kernel/linux/linux-renesas_%.bbappend")


def test_drp1_stub_or_real_switch():
    s = _t("recipes-kernel/linux/linux-renesas_%.bbappend")
    assert 'if [ "${ALP_DRP1_DT_ENABLE}" = "1" ]' in s
    assert "rz-opencva" in s and "meta-rz-codecs" in s


def test_udev_package_rides_packageconfig_drpai():
    s = _t("recipes-core/alp-sdk/alp-sdk_0.6.bb")
    line = next(x for x in s.splitlines() if x.startswith("PACKAGECONFIG[drpai]"))
    assert "alp-drpai-udev" in line
    assert "root drpai" in _t("recipes-bsp/alp-drpai-udev/files/alp-drpai-tmpfiles.conf")
    assert "f /run/alp/drpai.lock 0660 root drpai" in _t(
        "recipes-bsp/alp-drpai-udev/files/alp-drpai-tmpfiles.conf"
    )
    assert 'KERNEL=="rgnmm|rgnmmbuf", MODE="0660", GROUP="drpai"' in _t(
        "recipes-bsp/alp-drpai-udev/files/99-alp-drpai.rules"
    )
