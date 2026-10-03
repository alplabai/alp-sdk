# SPDX-License-Identifier: Apache-2.0
"""U-Boot's alp_sd_present (patch 0013) reads the SoM's SD1_SD1CD pad when the
carrier wires a card-detect switch. The pad is the SoM peripheral map's, the
switch is the carrier metadata's; sd1-cd.cfg, the bbappend gate and the Linux
cd-gpios must agree with them."""
import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
META = REPO / "meta-alp-sdk"
UBOOT = META / "recipes-bsp/u-boot"
CFG = UBOOT / "u-boot/sd1-cd.cfg"
PATCH = UBOOT / "u-boot/0013-rzv2n-dev-ALP-E1M-sd-card-detect.patch"
BBAPPEND = UBOOT / "u-boot_%.bbappend"
DTSI = META / "recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi"
PAD_MAP = REPO / "metadata/e1m_modules/v2n/renesas-peripheral-map.tsv"
CARRIER = REPO / "metadata/boards/e1m-x-evk.yaml"


def _bank_pin(port: str) -> tuple[int, int]:
    """'PA1' -> (10, 1); banks 0-9 are P0-P9, bank 10 is PA."""
    m = re.fullmatch(r"P([0-9A-Z])(\d)", port)
    return (int(m[1]) if m[1].isdigit() else ord(m[1]) - ord("A") + 10), int(m[2])


def _cfg() -> dict[str, str]:
    return dict(re.findall(r"^(CONFIG_\w+)=(\S+)$", CFG.read_text(), re.M))


def test_cfg_pad_is_the_som_pad_map_sd1cd():
    row = next(ln.split("	") for ln in PAD_MAP.read_text().splitlines() if ln.startswith("SD1_SD1CD	"))
    cfg = _cfg()
    assert (int(cfg["CONFIG_ALP_E1M_SD1_CD_BANK"]), int(cfg["CONFIG_ALP_E1M_SD1_CD_PIN"])) == _bank_pin(row[1])
    assert cfg["CONFIG_ALP_E1M_SD1_CD"] == "y"


def test_carrier_metadata_declares_the_switch_the_bbappend_defaults_to():
    wired = yaml.safe_load(CARRIER.read_text(encoding="utf-8"))["sd_slots"]["SD1"]["card_detect"]
    default = re.search(r'^ALP_CARRIER_SD1_CARD_DETECT \?= "(\d)"$', BBAPPEND.read_text(), re.M)[1]
    assert wired is (default == "1")


def test_patch_has_no_hard_coded_pad():
    # the pad comes from CONFIG_ALP_E1M_SD1_CD_BANK/_PIN, and the check is off without the flag
    text = PATCH.read_text()
    assert "ALP_SD1_CD_BANK" not in text.replace("CONFIG_ALP_E1M_SD1_CD_BANK", "")
    assert "IS_ENABLED(CONFIG_ALP_E1M_SD1_CD)" in text


def test_linux_cd_gpios_is_the_same_pad():
    letter, num = re.search(r"cd-gpios = <&pinctrl RZV2N_GPIO\((\w), (\d+)\) GPIO_ACTIVE_LOW>",
                            DTSI.read_text()).groups()
    cfg = _cfg()
    assert (int(cfg["CONFIG_ALP_E1M_SD1_CD_BANK"]), int(cfg["CONFIG_ALP_E1M_SD1_CD_PIN"])) == _bank_pin(f"P{letter}{num}")
