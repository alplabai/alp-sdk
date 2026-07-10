import shutil
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


def test_som_wrong_type_reports_clean_diagnostic_not_crash():
    """#602: a top-level `som:` scalar used to crash `_xref_pass` with
    an AttributeError (`'str' object has no attribute 'get'`) instead
    of reporting the schema violation."""
    c = validate_board_yaml(FIX_BAD / "ALP-B004-som-wrong-type.yaml")
    assert "ALP-B004" in _codes(c)
    assert c.has_errors()


def test_cores_wrong_type_reports_clean_diagnostic_not_crash():
    """#602: a top-level `cores:` scalar used to crash `_compat_pass`
    with an AttributeError (`'str' object has no attribute 'items'`)
    instead of reporting the schema violation."""
    c = validate_board_yaml(FIX_BAD / "ALP-B004-cores-wrong-type.yaml")
    assert "ALP-B004" in _codes(c)
    assert c.has_errors()


def test_som_wrong_type_standalone_validator_reports_clean_diagnostic():
    proc = subprocess.run(
        [
            sys.executable,
            str(REPO / "scripts" / "validate_board_yaml.py"),
            "--input",
            str(FIX_BAD / "ALP-B004-som-wrong-type.yaml"),
            "--no-color",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 1
    assert "Traceback" not in proc.stderr
    assert "ALP-B004" in proc.stderr


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
    # validate_board_yaml.py is now a thin wrapper over the shared validator +
    # orchestrator loader (entrypoint parity): it collapses the legacy 0/1/2/3
    # exit ladder to 0 (clean) / 1 (any error).  The ALP-B007 family-mismatch
    # diagnostic still fires via the orchestrator path -- only the numeric code
    # changes (3 -> 1).
    assert proc.returncode == 1
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
    assert "name" in proc.stderr


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


def _isolated_metadata_root(tmp_path: Path) -> Path:
    """Copy the repo's metadata/ tree + add a new board preset that
    exists ONLY in the copy, never in the repo's own metadata/ --
    proving the rich validator resolved the override root, not the
    repo default."""
    root = tmp_path / "metadata-copy"
    shutil.copytree(REPO / "metadata", root)
    (root / "boards" / "custom-local-604.yaml").write_text(
        "name: custom-local-604\n"
        "hosts_som_families: [alif-ensemble]\n"
        "populated: {}\n",
        encoding="utf-8",
    )
    return root


def test_metadata_root_honoured_by_rich_validator(tmp_path: Path):
    """#604: `validate_board_yaml(..., metadata_root=...)` used to
    ignore the override and always resolve SoM/preset/SoC metadata
    from the repo's own `metadata/`, so a customer's out-of-tree
    board preset failed ALP-B006 here even though the SAME preset
    validated fine through `load_board_yaml(metadata_root=...)`."""
    metadata_root = _isolated_metadata_root(tmp_path)
    board = tmp_path / "board.yaml"
    board.write_text(
        "som:\n  sku: E1M-AEN801\n"
        "preset: custom-local-604\n"
        "cores:\n  m55_hp:\n    app: .\n",
        encoding="utf-8",
    )

    # Custom preset does not exist in the repo's own metadata --
    # without the override this must fail ALP-B006.
    c_default = validate_board_yaml(board)
    assert "ALP-B006" in _codes(c_default)

    # With the override root, the preset resolves cleanly.
    c_override = validate_board_yaml(board, metadata_root=metadata_root)
    assert "ALP-B006" not in _codes(c_override)
    assert not c_override.has_errors()


def test_metadata_root_honoured_by_validate_board_yaml_script(tmp_path: Path):
    """#604 script-level parity: `validate_board_yaml.py --metadata-root`
    must thread the override into the rich validator's ALP-Bxxx pass,
    not just the orchestrator consistency pass."""
    metadata_root = _isolated_metadata_root(tmp_path)
    board = tmp_path / "board.yaml"
    board.write_text(
        "som:\n  sku: E1M-AEN801\n"
        "preset: custom-local-604\n"
        "cores:\n  m55_hp:\n    app: .\n",
        encoding="utf-8",
    )

    proc = subprocess.run(
        [
            sys.executable, str(SCRIPT),
            "--input", str(board),
            "--metadata-root", str(metadata_root),
            "--no-color",
        ],
        capture_output=True, text=True, check=False,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "ALP-B006" not in (proc.stdout + proc.stderr)
