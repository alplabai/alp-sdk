import subprocess
import sys
from pathlib import Path

import pytest
from alp_cli.validator import validate_board_yaml


REPO = Path(__file__).resolve().parents[2]
FIX_GOOD = Path(__file__).parent.parent / "fixtures" / "board_yaml_good"
FIX_BAD = Path(__file__).parent.parent / "fixtures" / "board_yaml_bad"
SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "validate_board_yaml.py"


def test_minimal_happy_path_emits_no_diagnostics():
    collector = validate_board_yaml(FIX_GOOD / "minimal.yaml")
    assert len(collector) == 0
    assert not collector.has_errors()


def _codes(collector) -> list[str]:
    return [d.code for d in collector]


def _script_schema_only(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--input", str(path), "--no-presets"],
        capture_output=True,
        text=True,
        check=False,
    )


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


def test_bad_sku_emits_ALP_B005():
    c = validate_board_yaml(FIX_BAD / "ALP-B005-bad-sku.yaml")
    assert "ALP-B005" in _codes(c)


def test_bad_preset_emits_ALP_B006():
    c = validate_board_yaml(FIX_BAD / "ALP-B006-bad-preset.yaml")
    assert "ALP-B006" in _codes(c)


def test_board_preset_family_mismatch_emits_ALP_B007():
    c = validate_board_yaml(FIX_BAD / "ALP-B007-board-preset-family.yaml")
    assert "ALP-B007" in _codes(c)


def test_standalone_validator_rejects_board_preset_family_mismatch():
    proc = subprocess.run(
        [
            sys.executable,
            str(REPO / "scripts" / "validate_board_yaml.py"),
            "--input",
            str(FIX_BAD / "ALP-B007-board-preset-family.yaml"),
        ],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 3
    assert "ALP-B007" in proc.stderr


def test_peripheral_not_on_soc_emits_ALP_B010():
    c = validate_board_yaml(FIX_BAD / "ALP-B010-peripheral-not-on-soc.yaml")
    assert "ALP-B010" in _codes(c)


def test_preset_mode_rejects_inline_name():
    path = FIX_BAD / "preset-with-name.yaml"

    c = validate_board_yaml(path)
    assert c.has_errors()

    proc = _script_schema_only(path)
    assert proc.returncode == 1
    assert "FAIL schema" in proc.stderr


@pytest.mark.parametrize("fixture", [
    "inline-populated-no-name.yaml",
    "inline-routes-no-name.yaml",
])
def test_inline_board_data_requires_name(fixture: str):
    path = FIX_BAD / fixture

    c = validate_board_yaml(path)
    assert "ALP-B001" in _codes(c)

    proc = _script_schema_only(path)
    assert proc.returncode == 1
    assert "name" in proc.stderr
