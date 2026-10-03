# SPDX-License-Identifier: Apache-2.0
"""The U-Boot environment location is stated in three places: the U-Boot
config fragment, the image's /etc/fw_env.config, and the provisioning
gate's boot-write ceiling. They must agree or fw_setenv writes where U-Boot
does not read, or a boot write erases the saved environment."""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from provision import gates  # noqa: E402

META = REPO / "meta-alp-sdk"
CFG = META / "recipes-bsp/u-boot/u-boot/uboot-env-emmc.cfg"
FW_ENV = META / "recipes-core/alp-system/files/fw_env.config"


def _cfg() -> dict[str, str]:
    return dict(m.groups() for m in re.finditer(r"^(CONFIG_\w+)=(\S+)$", CFG.read_text(), re.M))


def _fw_env() -> list[tuple[str, int, int]]:
    rows = [ln.split() for ln in FW_ENV.read_text().splitlines() if ln.strip() and not ln.startswith("#")]
    return [(dev, int(off, 0), int(size, 0)) for dev, off, size in rows]


def test_fw_env_config_matches_the_uboot_config():
    cfg, rows = _cfg(), _fw_env()
    assert cfg["CONFIG_SYS_MMC_ENV_PART"] == "2" and cfg["CONFIG_SYS_MMC_ENV_DEV"] == "0"
    assert cfg["CONFIG_SYS_REDUNDAND_ENVIRONMENT"] == "y"
    size = int(cfg["CONFIG_ENV_SIZE"], 0)
    # boot partition 2 is Linux boot1 of eMMC device 0
    assert rows == [("/dev/mmcblk0boot1", int(cfg["CONFIG_ENV_OFFSET"], 0), size),
                    ("/dev/mmcblk0boot1", int(cfg["CONFIG_ENV_OFFSET_REDUND"], 0), size)]


def test_the_two_copies_do_not_overlap_and_sit_above_every_boot_write():
    (_, a, size), (_, b, _) = _fw_env()
    assert abs(a - b) >= size
    assert min(a, b) == gates.BOOT_ENV_OFFSET
    fip_end_max = gates.FIP_SECTOR * 512 + gates.CM33_REGION_OFFSET
    assert min(a, b) >= fip_end_max
    assert max(a, b) + size <= 4 * 1024 * 1024      # smallest boot partition we assume
    assert a % 512 == 0 and b % 512 == 0 and size % 512 == 0


PATCH_0014 = META / "recipes-bsp/u-boot/u-boot/0014-rzv2n-dev-ALP-E1M-persistent-environment.patch"


def test_only_ota_variables_are_importable_from_the_saved_environment():
    """The write allowlist is what stops fw_setenv from changing how a unit
    boots: it must be on, and nothing that controls booting may be listed."""
    assert _cfg()["CONFIG_ENV_WRITEABLE_LIST"] == "y"
    names = set(re.findall(r"(\w+):sw", PATCH_0014.read_text()))
    assert {"upgrade_available", "bootcount", "mender_boot_part"} <= names
    assert not names & {"bootcmd", "bootargs", "bootdelay", "bootdelaykey", "preboot",
                        "bootcmd_check", "emmcload", "sd2load"}
    assert not any(n.startswith("bootstopkey") for n in names)
