"""Cross-check src/backends/power/alif_aipm_gen2.h against the E8 SVD.

The header's ALP_AIPM_SVD_* constants claim VBAT.RET_CTRL / ANA.WKUP_CTRL field
positions.  The C _Static_asserts prove the masks follow those constants; this
test proves the constants match the SVD file itself (issue #2784).
"""

import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/backends/power/alif_aipm_gen2.h"
SVD = ROOT / "metadata/svd/alif/AE822FA0E5597BS0_CM55_HE_View.svd"


def _fields(periph_name: str, register: str) -> dict[str, tuple[int, int]]:
    tree = ET.parse(SVD)
    for periph in tree.iter("peripheral"):
        if periph.findtext("name") != periph_name:
            continue
        for reg in periph.iter("register"):
            if reg.findtext("name") != register:
                continue
            out = {}
            for fld in reg.iter("field"):
                m = re.fullmatch(r"\[(\d+):(\d+)\]", fld.findtext("bitRange"))
                out[fld.findtext("name")] = (int(m.group(2)), int(m.group(1)) - int(m.group(2)) + 1)
            return out
    raise AssertionError(f"{periph_name}.{register} not found in {SVD.name}")


def _header_consts() -> dict[str, int]:
    text = HEADER.read_text(encoding="utf-8")
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"^#define\s+(ALP_AIPM_SVD_\w+)\s+(\d+)u", text, re.M)
    }


def test_ret_ctrl_bits_match_svd():
    fields = _fields("VBAT", "RET_CTRL")
    consts = _header_consts()
    checked = 0
    for name, val in consts.items():
        m = re.fullmatch(r"ALP_AIPM_SVD_RET_CTRL_(\w+)_BIT", name)
        if m:
            assert fields[m.group(1)] == (val, 1), name
            checked += 1
    assert checked == 6


def test_wkup_ctrl_bits_match_svd():
    fields = _fields("ANA", "WKUP_CTRL")
    consts = _header_consts()
    for fld in ("RTCA", "LPCMP", "BROWN_OUT"):
        assert fields[fld] == (consts[f"ALP_AIPM_SVD_WKUP_CTRL_{fld}_BIT"], 1), fld
    for fld in ("LPTIMER", "LPGPIO"):
        assert fields[fld] == (
            consts[f"ALP_AIPM_SVD_WKUP_CTRL_{fld}_LSB"],
            consts[f"ALP_AIPM_SVD_WKUP_CTRL_{fld}_WIDTH"],
        ), fld
