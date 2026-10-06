# SPDX-License-Identifier: Apache-2.0
"""The U-Boot environment location is a SoM fact (the preset's
on_module.emmc_boot block). The U-Boot config fragment, the image's
/etc/fw_env.config, mender.inc's MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_1/_2
and the provisioning gate's boot-write ceiling must all equal it, or fw_setenv
writes where U-Boot does not read, or a boot write erases the saved
environment."""
import re
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from provision import gates  # noqa: E402

META = REPO / "meta-alp-sdk"
CFG = META / "recipes-bsp/u-boot/u-boot/uboot-env-emmc.cfg"
FW_ENV = META / "recipes-core/alp-system/files/fw_env.config"
MENDER = META / "conf/distro/include/mender.inc"
PRESETS = sorted((REPO / "metadata/e1m_modules").glob("E1M-V2[NM]10[123].yaml"))


def _boot(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))["on_module"]["emmc_boot"]


def _cfg() -> dict[str, str]:
    return dict(m.groups() for m in re.finditer(r"^(CONFIG_\w+)=(\S+)$", CFG.read_text(encoding="utf-8"), re.M))


def _fw_env() -> list[tuple[str, int, int]]:
    rows = [ln.split() for ln in FW_ENV.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.startswith("#")]
    return [(dev, int(off, 0), int(size, 0)) for dev, off, size in rows]


def test_every_v2_preset_carries_the_same_layout():
    assert len(PRESETS) == 6
    assert len({yaml.safe_dump(_boot(p), sort_keys=True) for p in PRESETS}) == 1


@pytest.mark.parametrize("preset", PRESETS, ids=lambda p: p.stem)
def test_fw_env_config_and_cfg_match_the_preset(preset):
    env, cfg, rows = _boot(preset)["uboot_env"], _cfg(), _fw_env()
    assert int(cfg["CONFIG_SYS_MMC_ENV_PART"]) == env["part"] and cfg["CONFIG_SYS_MMC_ENV_DEV"] == "0"
    assert cfg["CONFIG_SYS_REDUNDAND_ENVIRONMENT"] == "y"
    assert int(cfg["CONFIG_ENV_SIZE"], 0) == env["size"]
    assert int(cfg["CONFIG_ENV_OFFSET"], 0) == env["offset"]
    assert int(cfg["CONFIG_ENV_OFFSET_REDUND"], 0) == env["offset_redund"]
    # boot partition 2 is Linux boot1 of eMMC device 0 (mmc0 = &sdhi0 in e1m-v2n-som.dtsi)
    dev = f"/dev/mmcblk0boot{env['part'] - 1}"
    assert rows == [(dev, env["offset"], env["size"]), (dev, env["offset_redund"], env["size"])]


def test_mender_offsets_match_the_preset():
    env = _boot(PRESETS[0])["uboot_env"]
    got = dict(re.findall(r'^(MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_[12]) \?= "(\w+)"$', MENDER.read_text(encoding="utf-8"), re.M))
    assert {k: int(v, 0) for k, v in got.items()} == {
        "MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_1": env["offset"],
        "MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_2": env["offset_redund"]}


def test_the_ev_som_alias_pins_mmc0_to_the_on_module_emmc():
    som = (META / "recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi").read_text(encoding="utf-8")
    carrier = (META / "recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi").read_text(encoding="utf-8")
    assert re.search(r"^\s*mmc0 = &sdhi0;", som, re.M)
    assert not re.search(r"^\s*mmc0 =", carrier, re.M)


@pytest.mark.parametrize("preset", PRESETS, ids=lambda p: p.stem)
def test_the_two_copies_do_not_overlap_and_sit_above_every_boot_write(preset):
    boot = _boot(preset)
    env = boot["uboot_env"]
    a, b, size = env["offset"], env["offset_redund"], env["size"]
    assert abs(a - b) >= size
    assert min(a, b) == gates.BOOT_ENV_OFFSET
    assert gates.FIP_SECTOR == boot["fip_sector"] and gates.BL2_MMC_SECTOR == boot["bl2_mmc_sector"]
    fip_end_max = boot["fip_sector"] * 512 + gates.CM33_REGION_OFFSET
    assert min(a, b) >= fip_end_max
    assert a % 512 == 0 and b % 512 == 0 and size % 512 == 0
    # TBD until the populated eMMC part's BOOT_SIZE_MULT is read from its datasheet
    if boot.get("partition_bytes", "TBD") != "TBD":
        assert max(a, b) + size <= boot["partition_bytes"]


PATCH_0016 = META / "recipes-bsp/u-boot/u-boot/0016-rzv2n-dev-ALP-E1M-persistent-environment.patch"


def test_only_ota_variables_are_importable_from_the_saved_environment():
    """The write allowlist is what stops fw_setenv from changing how a unit
    boots: it must be on, and nothing that controls booting may be listed."""
    assert _cfg()["CONFIG_ENV_WRITEABLE_LIST"] == "y"
    names = set(re.findall(r"(\w+):sw", PATCH_0016.read_text(encoding="utf-8")))
    assert {"upgrade_available", "bootcount", "mender_boot_part"} <= names
    assert not names & {"bootcmd", "bootargs", "bootdelay", "bootdelaykey", "preboot",
                        "bootcmd_check", "emmcload", "sd2load"}
    assert not any(n.startswith("bootstopkey") for n in names)
