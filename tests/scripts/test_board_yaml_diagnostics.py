from pathlib import Path

from alp_cli.validator import validate_board_yaml


FIX_GOOD = Path(__file__).parent.parent / "fixtures" / "board_yaml_good"
FIX_BAD = Path(__file__).parent.parent / "fixtures" / "board_yaml_bad"


def test_minimal_happy_path_emits_no_diagnostics():
    collector = validate_board_yaml(FIX_GOOD / "minimal.yaml")
    assert len(collector) == 0
    assert not collector.has_errors()
