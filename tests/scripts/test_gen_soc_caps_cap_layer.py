from pathlib import Path

import gen_soc_caps as gsc  # noqa: E402  (scripts/ on sys.path via conftest)
from .conftest import clang_format_text

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "include" / "alp" / "soc_caps.h"
CAP_H = REPO / "include" / "alp" / "cap.h"
CAP_C = REPO / "src" / "cap.c"


def test_header_contains_alp_has_macro_and_cap_definitions():
    text = gsc.emit()
    assert "#define ALP_HAS(cap)" in text
    # Every ALP_SOC_*_COUNT field should have a matching ALP_CAP_HW_* alias.
    assert "#define ALP_CAP_HW_I2C" in text
    assert "#define ALP_CAP_HW_SPI" in text
    assert "#define ALP_CAP_NPU_DRPAI" in text
    assert "#define ALP_CAP_HELIUM_MVE" in text


def test_soc_caps_h_matches_committed_file(tmp_path):
    """Generator output, clang-formatted, must be byte-identical to the
    committed header -- otherwise gen_soc_caps.py and include/alp/soc_caps.h
    have drifted and the `generated-files` CI gate should be catching it."""
    formatted = clang_format_text(tmp_path, "soc_caps.h", gsc.emit())
    assert formatted == HEADER.read_text(encoding="utf-8")


def test_cap_h_emits_enum_and_function_prototypes():
    text = gsc._emit_cap_h()
    assert "typedef enum" in text
    assert "ALP_CAP_ID_HW_I2C" in text
    assert "ALP_CAP_ID_COUNT" in text
    assert "bool alp_has(alp_cap_id_t cap);" in text
    assert "const char *alp_cap_name(alp_cap_id_t cap);" in text


def test_cap_h_matches_committed_file(tmp_path):
    formatted = clang_format_text(tmp_path, "cap.h", gsc._emit_cap_h())
    assert formatted == CAP_H.read_text(encoding="utf-8")


def test_cap_c_emits_table():
    text = gsc._emit_cap_c()
    assert "static const bool _cap_table" in text
    assert "alp_has" in text
    assert "alp_cap_name" in text


def test_cap_c_matches_committed_file(tmp_path):
    formatted = clang_format_text(tmp_path, "cap.c", gsc._emit_cap_c())
    assert formatted == CAP_C.read_text(encoding="utf-8")
