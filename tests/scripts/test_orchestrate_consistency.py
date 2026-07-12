# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- v0.6 P2.3 cross-field
consistency validator pass (OTA provider/target compatibility, boot
signing algorithm/family gating, TLS provider requirement, inference
arena-vs-heap sizing, power sleep-mode/wakeup-source pairing).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_consistency.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    _slice_alp_conf,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# v0.6 P2.3 -- cross-field validator pass
# ---------------------------------------------------------------------


def test_consistency_mender_on_zephyr_only_ok(tmp_path: Path) -> None:
    """Rule 1 (post-ADR-0009): ota.provider: mender on an all-Zephyr
    project is now valid -- Mender-MCU-client is the Zephyr-side
    dispatch.  Was rejected pre-ADR-0009; the v0.6 provider-driven
    dispatch flips this to ok.  The mender-mcu-client west group is
    not yet active (v0.7 follow-up), so the slice alp.conf emits the
    Kconfig settings as hint comments -- live CONFIG_MENDER_*=y lines
    would resolve to undefined-symbol warnings under Zephyr Kconfig
    today.  The validator still accepts the provider; only the emit
    shape is gated."""
    body = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: "off"

ota:
  provider: mender
  artifact_name: alp-aen-test
  server:
    url:    "https://hosted.mender.io"
    tenant: "${MENDER_TENANT_TOKEN}"
  poll_interval_s: 1800
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    # Zephyr Mender-MCU-client Kconfig must show up on the m55_hp slice
    # as hint comments while the west group is dormant.
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "# CONFIG_MENDER_MCU_CLIENT=y" in conf
    assert '# CONFIG_MENDER_SERVER_URL="https://hosted.mender.io"' in conf
    assert "# CONFIG_MENDER_UPDATE_POLL_INTERVAL=1800" in conf
    # And not as live settings -- the undefined-symbol form aborts the
    # twister build until the mender west group activates.
    for stem in ("CONFIG_MENDER_MCU_CLIENT", "CONFIG_MENDER_SERVER_URL",
                 "CONFIG_MENDER_UPDATE_POLL_INTERVAL"):
        assert f"\n{stem}=" not in conf, (
            f"{stem} must stay commented until mender-mcu-client lands "
            "in west.yml (otherwise Zephyr aborts on undefined symbol)"
        )


def test_consistency_mender_without_any_target_rejected(tmp_path: Path) -> None:
    """Rule 1: ota.provider: mender with NO yocto AND NO zephyr core
    (e.g. a project where every core is `off`) must still fail."""
    body = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: "off"
  m55_he:
    os: "off"

ota:
  provider: mender
  artifact_name: alp-aen-test
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mender" in msg
    assert "yocto" in msg.lower() or "zephyr" in msg.lower()


def test_consistency_hawkbit_requires_zephyr(tmp_path: Path) -> None:
    """Rule 1 (new in v0.6 dispatch): ota.provider: hawkbit requires
    at least one Zephyr core."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: "off"

ota:
  provider: hawkbit
  server:
    url: "https://hawkbit.example.com"
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="hawkbit"):
        load_board_yaml(path)


def test_consistency_hawkbit_on_zephyr_ok_with_kconfig(tmp_path: Path) -> None:
    """ota.provider: hawkbit on a Zephyr-targeted board.yaml validates
    and emits the Hawkbit DDI Kconfig on the Zephyr slice."""
    body = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: zephyr
    app: ./m55_hp
  m55_he:
    os: "off"

ota:
  provider: hawkbit
  server:
    url: "https://hawkbit.example.com"
  poll_interval_s: 600
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_HAWKBIT=y" in conf
    assert 'CONFIG_HAWKBIT_SERVER="https://hawkbit.example.com"' in conf
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=600" in conf
    # Negative: mender Kconfig must NOT bleed in.
    assert "CONFIG_MENDER_MCU_CLIENT" not in conf


def test_consistency_mcumgr_requires_zephyr(tmp_path: Path) -> None:
    """Rule 1 (new): ota.provider: mcumgr requires at least one Zephyr core."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: "off"

ota:
  provider: mcumgr
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError, match="mcumgr"):
        load_board_yaml(path)


def test_consistency_mcumgr_on_zephyr_emits_smp_kconfig(tmp_path: Path) -> None:
    """ota.provider: mcumgr enables the upstream SMP/MCUmgr Kconfig
    on every Zephyr slice; transport selection stays the app's call."""
    body = """
som:
  sku: E1M-AEN301

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

ota:
  provider: mcumgr
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_MCUMGR=y" in conf
    assert "CONFIG_MCUMGR_GRP_IMG=y" in conf
    assert "CONFIG_MCUMGR_GRP_OS=y" in conf


def test_consistency_mender_with_yocto_ok(tmp_path: Path) -> None:
    """Rule 1 happy path: V2N (A55 yocto + M33 zephyr) with mender OK."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
  m33_sm:
    os: zephyr
    app: ./m33

ota:
  provider: mender
  artifact_name: alp-v2n-test
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert project.ota.get("provider") == "mender"


def test_consistency_boot_signing_unsupported_for_family(
    tmp_path: Path,
) -> None:
    """Rule 2: AEN family with rsa2048 must fail (AEN only supports
    ECDSA-P256 + ed25519 under the OPTIGA Trust M attestation flow)."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

boot:
  method: mcuboot
  signing:
    algorithm: rsa2048
    key_file: keys/dev_rsa.pem
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "rsa2048" in msg
    assert "alif-ensemble" in msg
    assert "ecdsa_p256" in msg or "ed25519" in msg


def test_consistency_boot_signing_supported_aen_ecdsa(tmp_path: Path) -> None:
    """Rule 2 happy path: AEN + ecdsa_p256 OK."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/dev_ecdsa_p256.pem
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert (project.boot.get("signing") or {}).get("algorithm") == "ecdsa_p256"


def test_consistency_boot_signing_supported_v2n_rsa(tmp_path: Path) -> None:
    """Rule 2: V2N family accepts rsa2048 (different SoM family)."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33

boot:
  method: mcuboot
  signing:
    algorithm: rsa2048
    key_file: keys/dev_rsa.pem
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert (project.boot.get("signing") or {}).get("algorithm") == "rsa2048"


def test_consistency_tls_without_provider_rejected(tmp_path: Path) -> None:
    """Rule 3: iot.tls: true with no mbedtls / bearssl in libraries
    or extra_libraries must fail."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m33_sm" in msg
    assert "tls" in msg.lower()
    assert "mbedtls" in msg
    assert "bearssl" in msg


def test_consistency_tls_satisfied_by_curated_mbedtls(tmp_path: Path) -> None:
    """Rule 3 happy path: mbedtls in `libraries:` covers iot.tls."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    libraries: [mbedtls]
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "mbedtls" in project.cores["m33_sm"].libraries


def test_consistency_tls_satisfied_by_curated_bearssl(tmp_path: Path) -> None:
    """Rule 3: `bearssl` declared via the curated `libraries:` enum
    also satisfies iot.tls.  (bearssl + mbedtls are the two TLS
    providers rule 3 accepts.)"""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    libraries: [bearssl]
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "bearssl" in project.cores["m33_sm"].libraries


def test_consistency_arena_larger_than_heap_warns(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 4: arena_kib > heap_kib emits a WARN, but doesn't fail."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    memory: { heap_kib: 32 }
    inference: { default_arena_kib: 128 }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    err = capsys.readouterr().err
    assert "WARN" in err
    assert "default_arena_kib=128" in err
    assert "heap_kib=32" in err
    # Project loaded successfully (warning, not error).
    assert project.cores["m33_sm"].inference["default_arena_kib"] == 128


def test_consistency_arena_within_heap_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 4 happy path: arena fits in heap; no WARN emitted."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    memory: { heap_kib: 256 }
    inference: { default_arena_kib: 128 }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    # Rule 4 should not emit a WARN; G-4 partial-match WARN unrelated.
    assert "default_arena_kib" not in err


def test_consistency_sleep_mode_without_wakeup_warns(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5: sleep_mode != disabled without wakeup_sources emits a WARN."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power: { sleep_mode: deep }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "WARN" in err
    assert "sleep_mode=deep" in err
    assert "wakeup_sources" in err


def test_consistency_sleep_mode_with_wakeup_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5 happy path: sleep_mode with wakeup_sources declared -- no WARN."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power:
      sleep_mode: standby
      wakeup_sources: [uart, gpio]
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "sleep_mode" not in err


def test_consistency_sleep_mode_disabled_silent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str],
) -> None:
    """Rule 5: explicit `sleep_mode: disabled` (the default) doesn't WARN
    even when wakeup_sources is empty -- the device isn't sleeping."""
    body = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    power: { sleep_mode: disabled }
"""
    path = _write_board(tmp_path, body)
    load_board_yaml(path)
    err = capsys.readouterr().err
    assert "sleep_mode" not in err


