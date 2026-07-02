"""Click-runner tests for `alp new-som` (flag-driven / CI mode)."""

import json
from pathlib import Path

import jsonschema
import yaml
from click.testing import CliRunner

from alp_cli.main import cli

REPO = Path(__file__).resolve().parents[2]
SOM_SCHEMA = REPO / "metadata" / "schemas" / "som-preset-v1.schema.json"
SOC_SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"

# E1M-NX9555 deliberately matches the som-preset schema's existing
# NX9[0-9]{3} sku alternation, so the generated preset must validate
# with ZERO errors (not even the sanctioned sku-pattern exception).
BASE_FLAGS = [
    "new-som",
    "--sku", "E1M-NX9555",
    "--soc-ref", "nxp:imx9:imx95",
    "--family", "nxp-imx9",
]


def _run(tmp_path: Path, extra=()):
    return CliRunner().invoke(
        cli, BASE_FLAGS + ["--output-root", str(tmp_path)] + list(extra)
    )


def _validate(doc, schema_path: Path):
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    return sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(doc),
        key=lambda e: list(e.absolute_path),
    )


def test_new_som_preset_validates_against_som_preset_v1(tmp_path: Path):
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    preset_path = tmp_path / "metadata" / "e1m_modules" / "E1M-NX9555.yaml"
    assert preset_path.is_file()
    doc = yaml.safe_load(preset_path.read_text(encoding="utf-8"))
    errors = _validate(doc, SOM_SCHEMA)
    assert not errors, [e.message for e in errors]


def test_new_som_preset_uses_tbd_placeholders_not_invented_values(tmp_path: Path):
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    doc = yaml.safe_load(
        (tmp_path / "metadata" / "e1m_modules" / "E1M-NX9555.yaml")
        .read_text(encoding="utf-8")
    )
    assert doc["silicon_variant"] == "TBD"
    assert doc["memory"] == {"dram_mbit": "TBD", "flash_mbit": "TBD"}
    assert doc["on_module"]["pmic_main"] == "TBD"
    assert doc["mailbox"]["controller"] == "TBD"
    # Placeholder backend, placeholder core id -- nothing guessed.
    assert doc["inference"] == {"preferred_backend": "tbd"}
    assert list(doc["topology"].keys()) == ["tbd_core0"]
    assert doc["status"] == {"preliminary": True, "partial_hw_config": True}


def test_new_som_soc_skeleton_validates_against_soc_spec_v1(tmp_path: Path):
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    soc_path = tmp_path / "metadata" / "socs" / "nxp" / "imx9" / "imx95.json"
    assert soc_path.is_file()
    doc = json.loads(soc_path.read_text(encoding="utf-8"))
    errors = _validate(doc, SOC_SCHEMA)
    assert not errors, [e.message for e in errors]
    assert doc["ref"] == "nxp:imx9:imx95"
    assert doc["pending_reference_manual_ingestion"] is True
    assert doc["variants"][0]["alp_module_skus"] == ["E1M-NX9555"]


def test_new_som_prints_numbered_next_steps(tmp_path: Path):
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    assert "Next steps:" in result.output
    assert "1." in result.output
    assert "validate_metadata.py" in result.output
    assert "gen_soc_caps.py" in result.output
    assert "gen_board_header.py" in result.output
    assert "tests/zephyr/conformance" in result.output
    assert "porting-new-som.md" in result.output


def test_new_som_refuses_existing_sku_without_force(tmp_path: Path):
    first = _run(tmp_path)
    assert first.exit_code == 0, first.output
    second = _run(tmp_path)
    assert second.exit_code != 0
    assert "already exists" in second.output
    forced = _run(tmp_path, ["--force"])
    assert forced.exit_code == 0, forced.output


def test_new_som_leaves_existing_soc_spec_untouched(tmp_path: Path):
    soc_path = tmp_path / "metadata" / "socs" / "nxp" / "imx9" / "imx95.json"
    soc_path.parent.mkdir(parents=True)
    canned = '{"canned": true}\n'
    soc_path.write_text(canned, encoding="utf-8")
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    assert soc_path.read_text(encoding="utf-8") == canned
    assert "already present" in result.output


def test_new_som_ethos_u_requires_variant(tmp_path: Path):
    result = _run(tmp_path, ["--inference-backend", "ethos_u"])
    assert result.exit_code != 0
    assert "--ethos-u-variant" in result.output


def test_new_som_ethos_u_populates_npu_table_with_tbd_pairings(tmp_path: Path):
    result = _run(
        tmp_path, ["--inference-backend", "ethos_u", "--ethos-u-variant", "u65"]
    )
    assert result.exit_code == 0, result.output
    doc = yaml.safe_load(
        (tmp_path / "metadata" / "e1m_modules" / "E1M-NX9555.yaml")
        .read_text(encoding="utf-8")
    )
    assert doc["inference"]["ethos_u_variant"] == "u65"
    assert doc["inference"]["npu_population"] == [
        {"variant": "u65", "role": "TBD", "paired_with": "TBD"}
    ]
    errors = _validate(doc, SOM_SCHEMA)
    assert not errors, [e.message for e in errors]


def test_new_som_flags_out_of_pattern_sku_as_checklist_step(tmp_path: Path):
    # A brand-new family SKU that the schema pattern does not yet
    # accept: generation still succeeds, and extending the pattern is
    # surfaced as an explicit porting step (never silently skipped).
    result = CliRunner().invoke(
        cli,
        ["new-som", "--sku", "E1M-ST9101", "--soc-ref", "st:stm32mp2:mp257",
         "--family", "st-stm32mp2", "--output-root", str(tmp_path)],
    )
    assert result.exit_code == 0, result.output
    assert "som-preset-v1.schema.json" in result.output
    assert "E1M-ST9101" in result.output
    # Everything except the sku pattern must already validate.
    doc = yaml.safe_load(
        (tmp_path / "metadata" / "e1m_modules" / "E1M-ST9101.yaml")
        .read_text(encoding="utf-8")
    )
    errors = [e for e in _validate(doc, SOM_SCHEMA)
              if list(e.absolute_path) != ["sku"]]
    assert not errors, [e.message for e in errors]
