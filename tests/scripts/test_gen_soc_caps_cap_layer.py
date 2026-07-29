from pathlib import Path

import gen_soc_caps as gsc  # scripts/ on sys.path via conftest
from .conftest import clang_format_text

REPO = Path(__file__).resolve().parents[2]
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


def test_extract_unverified_peripherals_uses_per_key_list():
    """#936: a `peripherals_unverified` list names specific uncited keys."""
    soc = {"peripherals": {"pdm": 4, "pdm_lp": 4, "i2c": 4},
           "peripherals_unverified": ["pdm_lp", "pdm"]}
    assert gsc.extract_unverified_peripherals(soc) == ["pdm", "pdm_lp"]


def test_extract_unverified_peripherals_pending_rm_ingestion_covers_all_keys():
    """A whole-block-unaudited file (pending_reference_manual_ingestion) is
    treated as every `peripherals` key being unverified, even without its
    own `peripherals_unverified` list (e.g. E5, inherited wholesale from E7)."""
    soc = {"peripherals": {"i2c": 4, "spi": 4},
           "pending_reference_manual_ingestion": True}
    assert gsc.extract_unverified_peripherals(soc) == ["i2c", "spi"]


def test_extract_unverified_peripherals_absent_is_empty():
    assert gsc.extract_unverified_peripherals({"peripherals": {"i2c": 4}}) == []


def test_header_flags_alif_pdm_as_unverified():
    """Ground the emitted header against the real E3/E5 metadata (#936):
    the uniform, uncited pdm/pdm_lp value must surface as a comment, and
    E5's wholesale-inherited block must surface its own real key set."""
    text = gsc.emit()
    assert ("#if defined(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3)\n"
            "/* alif:ensemble:e3 */\n"
            "/* UNVERIFIED (no datasheet/DFP/HWRM citation): pdm, pdm_lp */\n") in text
    assert "UNVERIFIED (no datasheet/DFP/HWRM citation): adc_12bit" in text
