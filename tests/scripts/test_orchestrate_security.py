# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- security.psa: TF-M sysbuild
overlay emission + cross-field validation (emit_tfm_sysbuild_conf(), v0.6).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_security.py -v
"""

from __future__ import annotations

import shutil
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    Orchestrator,
    OrchestratorError,
    emit_tfm_sysbuild_conf,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# security.psa: emit + cross-field validation (v0.6)
# ---------------------------------------------------------------------


_AEN301_SECURITY_HAPPY = """
name: test-aen301-security
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: zephyr
    app: ./m55_he

storage:
  - name: psa_its
    size_kib: 64
    fs: raw
    flash_device: mram_main
  - name: psa_ps
    size_kib: 64
    fs: raw
    flash_device: mram_main

security:
  psa:
    persistent_slots: 32
    its_storage: psa_its
    ps_storage: psa_ps
    tfm: true
    attestation_root: optiga_trust_m
"""


def test_emit_tfm_sysbuild_conf_happy_path(tmp_path: Path) -> None:
    """Happy path: AEN301 + security.psa.tfm: true emits a TF-M
    sysbuild overlay with the expected SB_/CONFIG_ lines."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)

    # Core sysbuild knobs.
    assert "SB_CONFIG_TFM=y" in out
    assert "SB_CONFIG_TFM_BUILD_TYPE=Release" in out

    # PSA slot count + ITS/PS backing stores.
    assert "CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT=32" in out
    assert 'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="psa_its"' in out
    assert 'CONFIG_PSA_CRYPTO_PS_BACKING_STORE="psa_ps"' in out

    # OPTIGA attestation root surfaced.
    assert "CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y" in out
    # And the bridge-driver comment is present.
    assert "optiga_trust_m_bridge" in out


def test_emit_tfm_sysbuild_conf_absent_emits_nothing(tmp_path: Path) -> None:
    """No security.psa: block -> empty string + no file written."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    assert emit_tfm_sysbuild_conf(project) == ""


def test_emit_tfm_sysbuild_conf_tfm_false_emits_nothing(
    tmp_path: Path,
) -> None:
    """`security.psa:` block present but `tfm: false` -> empty.

    PSA Crypto runs non-secure-only (mbedTLS) -- no child image.
    """
    body = """
        name: test-aen301-no-tfm
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            persistent_slots: 8
            tfm: false
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert emit_tfm_sysbuild_conf(project) == ""


def test_security_psa_schema_round_trip(tmp_path: Path) -> None:
    """Schema-level: every documented security.psa.* field survives
    the loader and lands in BoardProject.security."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    psa = project.security["psa"]
    assert psa["persistent_slots"] == 32
    assert psa["its_storage"] == "psa_its"
    assert psa["ps_storage"] == "psa_ps"
    assert psa["tfm"] is True
    assert psa["attestation_root"] == "optiga_trust_m"


def test_security_psa_its_storage_resolves_to_memory_map(
    tmp_path: Path,
) -> None:
    """ITS backing store may reference a SoM memory_map region directly,
    not just a storage[] partition."""
    body = """
        name: test-aen301-its-memmap
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: mram_main
            tfm: true
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)
    assert 'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="mram_main"' in out


def test_security_psa_its_storage_unknown_reference_rejected(
    tmp_path: Path,
) -> None:
    """Unknown ITS backing-store ref -> OrchestratorError pointing at
    the offending YAML path."""
    body = """
        name: test-aen301-its-bad
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: nope_not_a_region
            tfm: true
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "security.psa.its_storage" in msg
    assert "nope_not_a_region" in msg


def test_security_psa_ps_storage_unknown_reference_rejected(
    tmp_path: Path,
) -> None:
    """Same rule for ps_storage when set."""
    body = """
        name: test-aen301-ps-bad
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            its_storage: mram_main
            ps_storage: definitely_missing
            tfm: true
    """
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "security.psa.ps_storage" in str(excinfo.value)


def test_security_psa_attestation_optiga_rejected_when_som_lacks_it(
    tmp_path: Path,
) -> None:
    """SoM without OPTIGA Trust M -> attestation_root: optiga_trust_m
    must reject with a helpful message.  Every shipping SKU carries
    OPTIGA, so this test stubs a synthetic E1M-AEN301 preset with the
    secure_element field DNI'd to exercise the rejection path."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs_alif = meta / "socs" / "alif" / "ensemble"
    schemas = meta / "schemas"
    for d in (e1m, socs_alif, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    real_soc_dir = real_meta / "socs" / "alif" / "ensemble"
    for soc_f in real_soc_dir.iterdir():
        shutil.copy(soc_f, socs_alif / soc_f.name)

    # Synthetic AEN301 variant with OPTIGA explicitly DNI'd.
    preset = yaml.safe_load(
        (real_meta / "e1m_modules" / "E1M-AEN301.yaml").read_text())
    preset["on_module"].pop("secure_element", None)
    preset["capabilities"].pop("optiga_trust_m", None)
    (e1m / "E1M-AEN301.yaml").write_text(yaml.safe_dump(preset),
                                          encoding="utf-8")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        name: test-no-optiga
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        security:
          psa:
            attestation_root: optiga_trust_m
            tfm: true
    """).lstrip("\n"), encoding="utf-8")

    with pytest.raises(OrchestratorError) as excinfo:
        alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    msg = str(excinfo.value)
    assert "attestation_root" in msg
    assert "optiga_trust_m" in msg
    assert "E1M-AEN301" in msg


def test_security_psa_materialise_writes_tfm_conf(tmp_path: Path) -> None:
    """Orchestrator._materialise_shared() writes build/sysbuild/tfm/tfm.conf
    when security.psa.tfm: true, and skips it otherwise."""
    path = _write_board(tmp_path, _AEN301_SECURITY_HAPPY)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    orch._materialise_shared()

    tfm_conf = build_root / "sysbuild" / "tfm" / "tfm.conf"
    assert tfm_conf.is_file(), "TF-M overlay not written"
    text = tfm_conf.read_text(encoding="utf-8")
    assert "SB_CONFIG_TFM=y" in text
    assert "CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT=32" in text


def test_security_psa_materialise_skips_when_absent(tmp_path: Path) -> None:
    """No security.psa: -> build/sysbuild/tfm/ is not created."""
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    build_root = tmp_path / "build"
    orch = Orchestrator(project, build_root, board_yaml=path)
    orch._materialise_shared()

    assert not (build_root / "sysbuild" / "tfm").exists()


def test_security_psa_build_type_inherits_from_boot(tmp_path: Path) -> None:
    """TF-M build type follows boot.build_type when set."""
    body = """
        name: test-aen301-debug
        som:
          sku: E1M-AEN301
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
        boot:
          method: mcuboot
          build_type: Debug
          signing:
            algorithm: ecdsa_p256
            key_file: keys/dev.pem
        security:
          psa:
            tfm: true
            its_storage: mram_main
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    out = emit_tfm_sysbuild_conf(project)
    assert "SB_CONFIG_TFM_BUILD_TYPE=Debug" in out

