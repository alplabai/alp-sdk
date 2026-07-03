"""Tests for the ADR 0018 curated third-party library layer.

Covers the four surfaces the feature adds:

  * the ``metadata/schemas/library-v1.schema.json`` schema + the
    ``validate_metadata.py`` semantic pass (accept the real manifests;
    reject a bad tier / licence / capability key / filename mismatch);
  * the orchestrator emit (top-level ``libraries:`` -> Zephyr Kconfig /
    Yocto IMAGE_INSTALL; incompatible + unknown selections raise a clear
    ``OrchestratorError``; zero-diff when unused);
  * the ``alp doctor`` libraries section.
"""

import json
import sys
import textwrap
from pathlib import Path

import jsonschema
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import load_board_yaml  # noqa: E402
from alp_orchestrate.kconfig import _slice_alp_conf, _slice_local_conf  # noqa: E402
from alp_orchestrate.models import OrchestratorError  # noqa: E402
from alp_orchestrate import libraries as liblayer  # noqa: E402

LIBRARY_SCHEMA = REPO / "metadata" / "schemas" / "library-v1.schema.json"
LIBRARIES_DIR = REPO / "metadata" / "libraries"

EXPECTED_LIBS = {"lvgl", "cmsis-dsp", "cmsis-nn", "nanopb", "zcbor"}


def _write_board(tmp: Path, body: str) -> Path:
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _validator() -> jsonschema.Draft202012Validator:
    schema = json.loads(LIBRARY_SCHEMA.read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def _valid_manifest() -> dict:
    """A minimal schema-valid manifest used as the base for reject cases."""
    return {
        "schema_version": 1,
        "name": "widget",
        "tier": "A",
        "version": "1.0.0",
        "license": "MIT",
        "integration": {"zephyr": {"kconfig": ["CONFIG_WIDGET=y"]}},
    }


# ---------------------------------------------------------------------
# Schema: the real manifests + the enumerated field set
# ---------------------------------------------------------------------

def test_all_expected_manifests_present() -> None:
    on_disk = {p.stem for p in LIBRARIES_DIR.glob("*.yaml")}
    assert EXPECTED_LIBS <= on_disk, f"missing manifests: {EXPECTED_LIBS - on_disk}"


@pytest.mark.parametrize("path", sorted(LIBRARIES_DIR.glob("*.yaml")),
                         ids=lambda p: p.stem)
def test_real_manifest_schema_valid(path: Path) -> None:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    errors = sorted(_validator().iter_errors(doc), key=lambda e: list(e.absolute_path))
    assert not errors, [e.message for e in errors]
    # name must match filename (the `libraries: [<name>]` token resolves by it).
    assert doc["name"] == path.stem


def test_schema_rejects_bad_tier() -> None:
    doc = _valid_manifest()
    doc["tier"] = "C"
    assert list(_validator().iter_errors(doc)), "tier C must be rejected"


def test_schema_rejects_bad_license() -> None:
    doc = _valid_manifest()
    doc["license"] = "GPL-3.0-only"
    assert list(_validator().iter_errors(doc)), "GPL licence must be rejected"


def test_schema_requires_an_integration_section() -> None:
    doc = _valid_manifest()
    doc["integration"] = {}
    assert list(_validator().iter_errors(doc)), "empty integration must be rejected"


def test_schema_rejects_unknown_top_level_key() -> None:
    doc = _valid_manifest()
    doc["fetch"] = "https://example.invalid/x.tar.gz"
    assert list(_validator().iter_errors(doc)), "additionalProperties must be false"


# ---------------------------------------------------------------------
# validate_metadata semantic pass
# ---------------------------------------------------------------------

def test_validate_metadata_semantics_accepts_real_manifests() -> None:
    import validate_metadata as vm
    failures = vm._check_library_semantics(sorted(LIBRARIES_DIR.glob("*.yaml")))
    assert failures == []


def test_validate_metadata_rejects_unknown_capability_key(tmp_path: Path) -> None:
    import validate_metadata as vm
    bad = tmp_path / "badcap.yaml"
    doc = _valid_manifest()
    doc["name"] = "badcap"
    doc["requires"] = {"capabilities": ["display"]}  # not a real SoC cap
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])
    assert failures, "unknown capability key must fail the semantic pass"
    assert "capabilities" in failures[0][1][0]


def test_validate_metadata_rejects_name_filename_mismatch(tmp_path: Path) -> None:
    import validate_metadata as vm
    bad = tmp_path / "onthedisk.yaml"
    doc = _valid_manifest()
    doc["name"] = "different"
    bad.write_text(yaml.safe_dump(doc), encoding="utf-8")
    failures = vm._check_library_semantics([bad])
    assert failures and "must match the manifest filename" in failures[0][1][0]


def test_capability_vocabulary_is_grounded() -> None:
    import validate_metadata as vm
    vocab = vm._capability_vocabulary()
    # A few keys that must exist per soc-spec-v1; and `display` must NOT
    # (there is no display capability -- the reason lvgl gates on RAM only).
    assert {"gpu2d", "dave2d", "cryptocell"} <= vocab
    assert "display" not in vocab


# ---------------------------------------------------------------------
# Emit: Zephyr Kconfig
# ---------------------------------------------------------------------

_V2N_LVGL = """
som:
  sku: E1M-V2N101
libraries: [lvgl]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_DSP = """
som:
  sku: E1M-V2N101
libraries: [cmsis-dsp]
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""

_V2N_NOLIB = """
som:
  sku: E1M-V2N101
cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


def test_emit_lvgl_zephyr_kconfig(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_LVGL))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_LVGL=y" in out
    assert "ADR 0018" in out
    assert "lvgl v9.5.0" in out  # version transcribed from the manifest


def test_emit_cmsis_dsp_zephyr_kconfig(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_DSP))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_CMSIS_DSP=y" in out
    assert "CONFIG_CMSIS_DSP_TRANSFORM=y" in out


def test_emit_zero_diff_without_libraries(tmp_path: Path) -> None:
    """A project that declares no `libraries:` must not gain the library block."""
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "ADR 0018" not in out
    assert "CONFIG_LVGL=y" not in out
    # Helper returns nothing for an unselected project (guards the guard).
    assert liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"]) == []


def test_emit_unknown_library_lists_available(tmp_path: Path) -> None:
    body = _V2N_NOLIB.replace("cores:", "libraries: [lvglx]\ncores:")
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.zephyr_kconfig_lines(project, project.cores["m33_sm"])
    msg = str(exc.value)
    assert "unknown library `lvglx`" in msg
    # lists the available manifests so the typo is self-correcting
    assert "lvgl" in msg and "cmsis-dsp" in msg


# ---------------------------------------------------------------------
# Emit: compatibility errors name the failing constraint
# ---------------------------------------------------------------------

def test_requires_min_ram_names_constraint(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    manifest = {"requires": {"min_ram_kib": 10 ** 9}}
    with pytest.raises(OrchestratorError) as exc:
        liblayer._check_requires("hog", manifest, project, liblayer.METADATA_ROOT)
    assert "min_ram_kib" in str(exc.value)


def test_requires_capability_names_constraint(tmp_path: Path) -> None:
    project = load_board_yaml(_write_board(tmp_path, _V2N_NOLIB))
    manifest = {"requires": {"capabilities": ["gpu2d"]}}  # V2N has no gpu2d cap
    with pytest.raises(OrchestratorError) as exc:
        liblayer._check_requires("needsgpu", manifest, project, liblayer.METADATA_ROOT)
    assert "gpu2d" in str(exc.value)


def test_incompatible_selection_not_wireable(tmp_path: Path) -> None:
    """cmsis-nn is Zephyr-only; a project whose only live core runs yocto
    cannot wire it -- resolve_selection must reject naming the mismatch."""
    body = """
    som:
      sku: E1M-V2N101
    libraries: [cmsis-nn]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
      m33_sm:
        os: "off"
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    with pytest.raises(OrchestratorError) as exc:
        liblayer.resolve_selection(project)
    assert "cannot be wired" in str(exc.value)


# ---------------------------------------------------------------------
# Emit: Yocto IMAGE_INSTALL
# ---------------------------------------------------------------------

def test_emit_lvgl_yocto_image_install(tmp_path: Path) -> None:
    body = """
    som:
      sku: E1M-V2N101
    libraries: [lvgl]
    cores:
      a55_cluster:
        os: yocto
        app: ./linux
        image: alp-image-edge
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    out = _slice_local_conf(project, project.cores["a55_cluster"])
    assert 'IMAGE_INSTALL:append = " lvgl"' in out


# ---------------------------------------------------------------------
# alp doctor
# ---------------------------------------------------------------------

def test_doctor_libraries_none_without_project(monkeypatch, tmp_path: Path) -> None:
    from alp_cli import doctor
    monkeypatch.chdir(tmp_path)  # no board.yaml here
    assert doctor._check_libraries() is None


def test_doctor_libraries_reports_selection(monkeypatch, tmp_path: Path) -> None:
    from alp_cli import doctor
    _write_board(tmp_path, _V2N_LVGL)
    monkeypatch.chdir(tmp_path)
    result = doctor._check_libraries()
    assert result is not None
    assert result.status == doctor.PASS
    assert "lvgl" in result.message
    assert "tier A" in result.message and "MIT" in result.message


def test_doctor_libraries_none_when_empty(monkeypatch, tmp_path: Path) -> None:
    from alp_cli import doctor
    _write_board(tmp_path, _V2N_NOLIB)
    monkeypatch.chdir(tmp_path)
    assert doctor._check_libraries() is None
