# SPDX-License-Identifier: Apache-2.0
"""U-Boot's alp_sd_present (patch 0013) hard-codes the microSD card-detect pin;
Linux declares the same net as cd-gpios in e1m-x-evk.dtsi. They must name the
same pin, or one side reads a different net than the other."""
import re
from pathlib import Path

META = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
PATCH = META / "recipes-bsp/u-boot/u-boot/0013-rzv2n-dev-ALP-E1M-sd-card-detect.patch"
DTSI = META / "recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi"


def test_uboot_card_detect_pin_matches_the_dtsi_cd_gpios():
    bank = int(re.search(r"#define ALP_SD1_CD_BANK\s+(\d+)", PATCH.read_text()).group(1))
    pin = int(re.search(r"#define ALP_SD1_CD_PIN\s+(\d+)", PATCH.read_text()).group(1))
    letter, num = re.search(r"cd-gpios = <&pinctrl RZV2N_GPIO\((\w), (\d+)\) GPIO_ACTIVE_LOW>",
                            DTSI.read_text()).groups()
    # banks 0-9 are P0-P9, bank 10 is PA
    assert bank == (int(letter) if letter.isdigit() else ord(letter) - ord("A") + 10)
    assert pin == int(num)
