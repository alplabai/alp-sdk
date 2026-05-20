from pathlib import Path

from alp_cli.validator import validate_board_yaml


FIX_GOOD = Path(__file__).parent.parent / "fixtures" / "board_yaml_good"
FIX_BAD = Path(__file__).parent.parent / "fixtures" / "board_yaml_bad"


def test_minimal_happy_path_emits_no_diagnostics():
    collector = validate_board_yaml(FIX_GOOD / "minimal.yaml")
    assert len(collector) == 0
    assert not collector.has_errors()


def _codes(collector) -> list[str]:
    return [d.code for d in collector]


def test_missing_required_key_emits_ALP_B001():
    c = validate_board_yaml(FIX_BAD / "ALP-B001-missing-required.yaml")
    assert "ALP-B001" in _codes(c)


def test_unknown_key_emits_ALP_B002_with_didyoumean():
    c = validate_board_yaml(FIX_BAD / "ALP-B002-unknown-key.yaml")
    diags = [d for d in c if d.code == "ALP-B002"]
    assert diags, "ALP-B002 expected"
    assert "diagnostics" in (diags[0].hint or "")


def test_bad_enum_emits_ALP_B003():
    c = validate_board_yaml(FIX_BAD / "ALP-B003-bad-enum.yaml")
    diags = [d for d in c if d.code == "ALP-B003"]
    assert diags, "ALP-B003 expected"
    assert "enum" in (diags[0].hint or "") or "one of" in (diags[0].hint or "")


def test_wrong_type_emits_ALP_B004():
    c = validate_board_yaml(FIX_BAD / "ALP-B004-wrong-type.yaml")
    assert "ALP-B004" in _codes(c)
