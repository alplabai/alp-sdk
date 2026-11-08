"""Cross-check the register constants of src/backends/power/alif_se_power.c against the E8 SVD.

The STOP backend re-asserts VBAT.RET_CTRL and ANA.VBAT_ANA_REG1 bits after the
Secure Enclave has written its profile (issue #2784, U7).  A bit position that is
off by one would set the wrong retention bit silently, so each ALP_ALIF_SE_SVD_*
constant is proven against the SVD here, and the register offsets the hardware
seam (alif_se_power_hw.c) uses are proven against the SVD register list.
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / "src/backends/power/alif_se_power.c"
HW = ROOT / "src/backends/power/alif_se_power_hw.c"
SVD = ROOT / "metadata/svd/alif/AE822FA0E5597BS0_CM55_HE_View.svd"


def _register(periph_name: str, register: str):
    tree = ET.parse(SVD)
    for periph in tree.iter("peripheral"):
        if periph.findtext("name") != periph_name:
            continue
        for reg in periph.iter("register"):
            if reg.findtext("name") == register:
                return reg
    raise AssertionError(f"{periph_name}.{register} not found in {SVD.name}")


def _fields(reg) -> dict[str, tuple[int, int]]:
    out = {}
    for fld in reg.iter("field"):
        m = re.fullmatch(r"\[(\d+):(\d+)\]", fld.findtext("bitRange"))
        assert m, fld.findtext("name")
        out[fld.findtext("name")] = (int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1)
    return out


def _consts() -> dict[str, int]:
    text = BACKEND.read_text(encoding="utf-8")
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"^#define\s+(ALP_ALIF_SE_SVD_\w+)\s+(\d+)u", text, re.M)
    }


def test_ana_reg1_bits_match_svd():
    fields = _fields(_register("ANA", "VBAT_ANA_REG1"))
    consts = _consts()
    for name in ("RET_LDO_VBAT_EN", "RET_LDO_VDDMAIN_EN", "XTAL32K_EN"):
        assert fields[name] == (consts[f"ALP_ALIF_SE_SVD_ANA_REG1_{name}_BIT"], 1), name
    assert fields["XTAL32K_CAP_CONT"] == (
        consts["ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_LSB"],
        consts["ALP_ALIF_SE_SVD_ANA_REG1_XTAL32K_CAP_CONT_WIDTH"],
    )


def test_misc_ctrl_sel_32k_matches_svd():
    fields = _fields(_register("ANA", "MISC_CTRL"))
    assert fields["SEL_32K"] == (_consts()["ALP_ALIF_SE_SVD_ANA_MISC_SEL_32K_BIT"], 1)


def test_cap_cont_max_is_the_field_maximum():
    _, width = _fields(_register("ANA", "VBAT_ANA_REG1"))["XTAL32K_CAP_CONT"]
    m = re.search(r"^#define\s+XTAL32K_CAP_CONT_MAX\s+(\d+)u", BACKEND.read_text(encoding="utf-8"), re.M)
    assert m and int(m.group(1)) == (1 << width) - 1


def test_register_offsets_used_by_the_hw_seam():
    text = HW.read_text(encoding="utf-8")
    offs = {
        "HW_RET_CTRL": ("VBAT", "RET_CTRL"),
        "HW_ANA_MISC": ("ANA", "MISC_CTRL"),
        "HW_ANA_REG1": ("ANA", "VBAT_ANA_REG1"),
    }
    for sym, (periph, reg) in offs.items():
        m = re.search(rf"^#define\s+{sym}\s+\(HW_\w+_BASE \+ (0x[0-9A-Fa-f]+)u\)", text, re.M)
        assert m, sym
        want = int(_register(periph, reg).findtext("addressOffset"), 16)
        assert int(m.group(1), 16) == want, sym


def test_rtss_core_ctrl_bits_match_svd():
    text = HW.read_text(encoding="utf-8")
    hdr = (ROOT / "src/backends/power/alif_se_power_hw.h").read_text(encoding="utf-8")
    cores = (("RTSS_HE_CTRL", 0x10, "0x1A604010u"), ("RTSS_HP_CTRL", 0x0, "0x1A604000u"))
    for name, off, want in cores:
        reg = _register("AON", name)
        assert int(reg.findtext("addressOffset"), 16) == off, name
        fields = _fields(reg)
        assert fields["COLD_WAKEUP"] == (0, 1), name
        assert fields["WIC"] == (8, 2), name
        # the backend picks the core's own pair at build time: one #define per core
        assert re.search(rf"^#define\s+HW_CORE_CTRL\s+{want}", text, re.M), name
    assert re.search(r"HE_CTRL_COLD_WAKEUP\s+BIT\(0\)", text)
    assert re.search(r"ALIF_SE_CTRL_WIC_EN\s+\(1u << 8\)", hdr)
    assert re.search(r"ALIF_SE_CTRL_WIC_IWIC\s+\(1u << 9\)", hdr)
