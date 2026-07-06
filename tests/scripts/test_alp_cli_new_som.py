"""Click-runner tests for `alp new-som` (flag-driven / CI mode)."""

import json
import shutil
import subprocess
import sys
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


def test_new_som_rejects_bad_sku_writing_nothing(tmp_path: Path):
    result = CliRunner().invoke(
        cli,
        ["new-som", "--sku", "e1m-lower", "--soc-ref", "nxp:imx9:imx95",
         "--family", "nxp-imx9", "--output-root", str(tmp_path)],
    )
    assert result.exit_code != 0
    assert "E1M-<UPPERCASE>" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_rejects_bad_soc_ref_writing_nothing(tmp_path: Path):
    result = CliRunner().invoke(
        cli,
        ["new-som", "--sku", "E1M-NX9555", "--soc-ref", "NXP/imx95",
         "--family", "nxp-imx9", "--output-root", str(tmp_path)],
    )
    assert result.exit_code != 0
    assert "<vendor>:<family>:<part>" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_rejects_bad_family_writing_nothing(tmp_path: Path):
    # Uppercase is not a slug.
    result = CliRunner().invoke(
        cli,
        ["new-som", "--sku", "E1M-NX9555", "--soc-ref", "nxp:imx9:imx95",
         "--family", "NXP-IMX9", "--output-root", str(tmp_path)],
    )
    assert result.exit_code != 0
    assert "lowercase slug" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_rejects_empty_cores_writing_nothing(tmp_path: Path):
    # `--cores ""` (and whitespace-only values) must be rejected up
    # front -- the empty tuple would otherwise dodge the None default
    # and scaffold a preset with an empty topology.
    for value in ("", " , "):
        result = _run(tmp_path, ["--cores", value])
        assert result.exit_code != 0, result.output
        assert "no core ids" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_ethos_u_rejection_leaves_no_files(tmp_path: Path):
    # Validation happens before any write: a rejected invocation must
    # not leave a half-written scaffold behind.
    result = _run(tmp_path, ["--inference-backend", "ethos_u"])
    assert result.exit_code != 0
    assert not (tmp_path / "metadata").exists()


def test_new_som_non_tty_missing_flags_fail_fast():
    # CliRunner stdin is a pipe (not a TTY): missing required flags must
    # fail immediately, naming each one, instead of dropping into the
    # questionary prompts and dying with an opaque "Aborted!".
    result = CliRunner().invoke(cli, ["new-som", "--sku", "E1M-NX9555"])
    assert result.exit_code != 0
    assert "--soc-ref" in result.output
    assert "--family" in result.output
    # --sku WAS provided, so it must not be reported missing.
    assert "--sku" not in result.output
    assert "Aborted" not in result.output


def test_new_som_non_tty_lists_only_missing_flags():
    result = CliRunner().invoke(cli, ["new-som"])
    assert result.exit_code != 0
    for flag in ("--sku", "--soc-ref", "--family"):
        assert flag in result.output


def test_new_som_dry_run_writes_nothing(tmp_path: Path):
    result = _run(tmp_path, ["--dry-run"])
    assert result.exit_code == 0, result.output
    assert "Would create" in result.output
    assert "E1M-NX9555.yaml" in result.output
    assert "imx95.json" in result.output
    assert "nothing was written" in result.output
    assert "Next steps:" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_dry_run_still_validates(tmp_path: Path):
    result = _run(tmp_path, ["--dry-run", "--default-hw-rev", "r99"])
    assert result.exit_code != 0
    assert "r99" in result.output


def test_new_som_escapes_quotes_in_display_name(tmp_path: Path):
    tricky = 'He said "hi" \\ done'
    result = _run(tmp_path, ["--display-name", tricky])
    assert result.exit_code == 0, result.output
    doc = yaml.safe_load(
        (tmp_path / "metadata" / "e1m_modules" / "E1M-NX9555.yaml")
        .read_text(encoding="utf-8")
    )
    # The YAML must both parse and round-trip the exact string.
    assert doc["display_name"] == tricky
    errors = _validate(doc, SOM_SCHEMA)
    assert not errors, [e.message for e in errors]


def test_new_som_rejects_control_chars_in_display_name(tmp_path: Path):
    result = _run(tmp_path, ["--display-name", "two\nlines"])
    assert result.exit_code != 0
    assert "control characters" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_rejects_unknown_default_board(tmp_path: Path):
    result = _run(tmp_path, ["--default-board", "NOPE-BOARD"])
    assert result.exit_code != 0
    assert "metadata/boards/" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_rejects_unresolvable_default_hw_rev(tmp_path: Path):
    # E1M-NX9555 maps to the imx93 family dir, whose hw-revisions.yaml
    # exists in the SDK checkout -- an unknown rev must fail here, at
    # scaffold time, not later at validate time.
    result = _run(tmp_path, ["--default-hw-rev", "r99"])
    assert result.exit_code != 0
    assert "hw-revisions.yaml" in result.output
    assert not (tmp_path / "metadata").exists()


def test_new_som_defers_hw_rev_check_for_brand_new_family(tmp_path: Path):
    # E1M-ST9101 has no SKU-prefix -> family-dir mapping yet, so there
    # is no hw-revisions file to check against: the scaffold must still
    # succeed and keep the resolution as an explicit checklist step.
    result = CliRunner().invoke(
        cli,
        ["new-som", "--sku", "E1M-ST9101", "--soc-ref", "st:stm32mp2:mp257",
         "--family", "st-stm32mp2", "--default-hw-rev", "r7",
         "--output-root", str(tmp_path)],
    )
    assert result.exit_code == 0, result.output
    assert "default_hw_rev 'r7' resolves" in result.output


def test_new_som_omits_hw_rev_checklist_step_when_validated(tmp_path: Path):
    result = _run(tmp_path)
    assert result.exit_code == 0, result.output
    # The rev resolved against the family hw-revisions file up front,
    # so the checklist no longer defers it.
    assert "resolves in" not in result.output


def _clone_metadata_gates(repo_root: Path, tmp_repo: Path) -> None:
    """Copy the real metadata tree + gate scripts into a scratch repo.

    Both gate scripts resolve paths relative to their own location, so
    running the copies exercises the exact CI command set against a
    tree that additionally contains the scaffolded output.
    """
    shutil.copytree(repo_root / "metadata", tmp_repo / "metadata")
    (tmp_repo / "scripts").mkdir(parents=True)
    for script in ("validate_metadata.py", "check_inference_backend_parity.py"):
        shutil.copy(repo_root / "scripts" / script, tmp_repo / "scripts" / script)
    select_c = tmp_repo / "src" / "backends" / "inference" / "alp_model_select.c"
    select_c.parent.mkdir(parents=True)
    shutil.copy(
        repo_root / "src" / "backends" / "inference" / "alp_model_select.c",
        select_c,
    )


def test_scaffold_passes_real_metadata_validate_and_parity_gates(tmp_path: Path):
    # THE mergeability contract: a freshly scaffolded SoM committed
    # as-is must pass the pr-metadata-validate command set --
    # validate_metadata.py plus check_inference_backend_parity.py (the
    # `tbd` backend rides on status.preliminary: true).
    repo = tmp_path / "repo"
    _clone_metadata_gates(REPO, repo)
    result = _run(repo)
    assert result.exit_code == 0, result.output
    for script in ("validate_metadata.py", "check_inference_backend_parity.py"):
        proc = subprocess.run(
            [sys.executable, str(repo / "scripts" / script)],
            capture_output=True, text=True,
        )
        assert proc.returncode == 0, f"{script}:\n{proc.stdout}\n{proc.stderr}"


def test_parity_gate_rejects_tbd_once_preliminary_cleared(tmp_path: Path):
    # Graduation guard: clearing status.preliminary without replacing
    # the `tbd` placeholder backend must fail the parity gate.
    repo = tmp_path / "repo"
    _clone_metadata_gates(REPO, repo)
    result = _run(repo)
    assert result.exit_code == 0, result.output
    preset = repo / "metadata" / "e1m_modules" / "E1M-NX9555.yaml"
    text = preset.read_text(encoding="utf-8")
    assert "preliminary:          true" in text
    preset.write_text(
        text.replace("preliminary:          true", "preliminary:          false"),
        encoding="utf-8",
    )
    proc = subprocess.run(
        [sys.executable,
         str(repo / "scripts" / "check_inference_backend_parity.py")],
        capture_output=True, text=True,
    )
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "status.preliminary" in proc.stderr


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
