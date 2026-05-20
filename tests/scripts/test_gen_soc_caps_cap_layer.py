import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_soc_caps.py"
HEADER = REPO / "include" / "alp" / "soc_caps.h"


def test_header_contains_alp_has_macro_and_cap_definitions():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = HEADER.read_text(encoding="utf-8")
    assert "#define ALP_HAS(cap)" in text
    # Every ALP_SOC_*_COUNT field should have a matching ALP_CAP_HW_* alias.
    assert "#define ALP_CAP_HW_I2C" in text
    assert "#define ALP_CAP_HW_SPI" in text
    assert "#define ALP_CAP_NPU_DRPAI" in text
    assert "#define ALP_CAP_HELIUM_MVE" in text


CAP_H = REPO / "include" / "alp" / "cap.h"
CAP_C = REPO / "src" / "cap.c"


def test_cap_h_emits_enum_and_function_prototypes():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = CAP_H.read_text(encoding="utf-8")
    assert "typedef enum" in text
    assert "ALP_CAP_ID_HW_I2C" in text
    assert "ALP_CAP_ID_COUNT" in text
    assert "bool alp_has(alp_cap_id_t cap);" in text
    assert "const char *alp_cap_name(alp_cap_id_t cap);" in text


def test_cap_c_emits_table():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = CAP_C.read_text(encoding="utf-8")
    assert "static const bool _cap_table" in text
    assert "alp_has" in text
    assert "alp_cap_name" in text
