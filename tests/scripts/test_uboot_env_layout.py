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
PRESETS = sorted((REPO / "metadata/e1m_modules").glob("E1M-V2[NM]10[1-6].yaml"))


def _boot(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))["on_module"]["emmc_boot"]


def _cfg() -> dict[str, str]:
    return dict(m.groups() for m in re.finditer(r"^(CONFIG_\w+)=(\S+)$", CFG.read_text(encoding="utf-8"), re.M))


def _fw_env() -> list[tuple[str, int, int]]:
    rows = [ln.split() for ln in FW_ENV.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.startswith("#")]
    return [(dev, int(off, 0), int(size, 0)) for dev, off, size in rows]


def test_every_v2_preset_carries_the_same_layout():
    assert len(PRESETS) == 12
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


UBOOT_PATCHES = META / "recipes-bsp/u-boot/u-boot"
PATCH_0018 = UBOOT_PATCHES / "0018-rzv2n-dev-ALP-E1M-mender-ab-and-signed-fit-boot.patch"


def _flags_list() -> list[tuple[str, str]]:
    """(name, flags) of the allowlist as the patch series leaves it: the
    "name:flags,..." string lines the last patch that adds any of them
    carries (a later patch may re-type the entries and leave the #define as
    context)."""
    entry = re.compile(r"^\w+:[a-z]{2}(,\w+:[a-z]{2})*,?$")
    body = None
    for patch in sorted(UBOOT_PATCHES.glob("00*.patch")):
        added = [m.group(1) for ln in patch.read_text(encoding="utf-8").splitlines()
                 if ln.startswith("+\t") and (m := re.fullmatch(r'\+\t"([^"]*)"(?: \\)?', ln)) and entry.match(m.group(1))]
        if added:
            body = "".join(added)
    assert body, "no CFG_ENV_FLAGS_LIST_STATIC strings in the patch series"
    return [tuple(e.split(":")) for e in body.split(",") if e]


def test_every_allowlist_entry_uses_a_valid_u_boot_flag():
    """env/flags.c: type is one of s d x b (+ i m with CONFIG_NET), access one
    of a r o c (+ w with CONFIG_ENV_WRITEABLE_LIST).  An unknown letter is not
    an error in U-Boot, it silently disables the type check."""
    for name, flags in _flags_list():
        assert len(flags) == 2 and flags[0] in "sdxbim" and flags[1] in "arocw", (name, flags)


def test_only_ota_variables_are_importable_from_the_saved_environment():
    """The write allowlist is what stops fw_setenv from changing how a unit
    boots: it must be on, and nothing that controls booting may be listed."""
    assert _cfg()["CONFIG_ENV_WRITEABLE_LIST"] == "y"
    flags = dict(_flags_list())
    names = set(flags)
    assert {"upgrade_available", "bootcount", "mender_boot_part"} <= names
    assert not names & {"bootcmd", "bootargs", "bootdelay", "bootdelaykey", "preboot",
                        "bootcmd_check", "emmcload", "sd2load"}
    assert not any(n.startswith("bootstopkey") for n in names)
    # what reaches a Mender decision is typed: the saved string is a number
    assert {flags[n][0] for n in ("upgrade_available", "bootcount", "mender_boot_part",
                                  "mender_saveenv_canary")} == {"d"}
    assert flags["mender_boot_part_hex"][0] == "x"


def test_mender_env_device_is_the_one_u_boot_reads():
    """meta-mender derives /etc/fw_env.config's device from the env part: 2 is
    /dev/mmcblk0boot1, the default 0 is the user area U-Boot never reads."""
    env = _boot(PRESETS[0])["uboot_env"]
    text = MENDER.read_text(encoding="utf-8")
    assert re.search(rf'^MENDER_UBOOT_CONFIG_SYS_MMC_ENV_PART \?= "{env["part"]}"$', text, re.M)
    assert re.search(rf'^BOOTENV_SIZE \?= "{hex(env["size"])}"$', text, re.M)


def test_mender_bootcmd_picks_the_slot_from_literals_only():
    """The saved environment is writable from Linux: the slot numbers in the
    ALP bootcmd must be literals equal to mender.inc's A/B partitions, and the
    saved string must never be pasted into the mmc spec or the cmdline."""
    mender = MENDER.read_text(encoding="utf-8")
    part = {s: re.search(rf'^MENDER_ROOTFS_PART_{s} \?= "\$\{{MENDER_STORAGE_DEVICE_BASE\}}(\d+)"', mender, re.M).group(1)
            for s in "AB"}
    patch = PATCH_0018.read_text(encoding="utf-8")
    for s in "AB":
        assert f"setenv alp_mmc 0:{part[s]};setenv alp_root /dev/mmcblk0p{part[s]}" in patch
    added = "\n".join(ln for ln in patch.splitlines() if ln.startswith("+") and not ln.startswith("+ *"))
    assert not re.search(r"alp_mmc \$\{|alp_root \$\{|setenv alp_\w+ \$\{|mender_kernel_root|mender_uboot_root", added)
