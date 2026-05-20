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
