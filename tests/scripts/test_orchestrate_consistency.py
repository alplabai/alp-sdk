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
    # The URI is decomposed: CONFIG_HAWKBIT_SERVER is a BARE host (it feeds
    # zsock_getaddrinfo() / the TLS SNI name / the HTTP Host: header), the
    # scheme becomes the port + TLS knobs.  alplabai/tan-cli#558.
    assert 'CONFIG_HAWKBIT_SERVER="hawkbit.example.com"' in conf
    assert 'CONFIG_HAWKBIT_SERVER="https://hawkbit.example.com"' not in conf
    assert "CONFIG_HAWKBIT_PORT=443" in conf
    assert "CONFIG_HAWKBIT_USE_TLS=y" in conf
    # 600 s == 10 min; the symbol is declared in MINUTES.  alplabai/tan-cli#557.
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=10" in conf
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=600" not in conf
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

libraries:
  - name: mbedtls
    cores: [m33_sm]

cores:
  m33_sm:
    os: zephyr
    app: ./m33
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

libraries:
  - name: bearssl
    cores: [m33_sm]

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "bearssl" in project.cores["m33_sm"].libraries


def test_consistency_tls_satisfied_by_project_wide_mbedtls(
    tmp_path: Path,
) -> None:
    """Rule 3 (#1359 follow-up): a project-wide `libraries: [mbedtls]`
    (no `cores:` key) must satisfy `iot.tls: true` exactly like the
    `cores:`-scoped spelling does -- rule 3 used to read only
    `slice_.libraries`, so this legal board.yaml raised
    `OrchestratorError` even though mbedtls is genuinely in scope on
    every core via the project-wide channel."""
    body = """
som:
  sku: E1M-V2N101

libraries: [mbedtls]

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert "mbedtls" in project.libraries


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




# ---------------------------------------------------------------------
# OTA -- Hawkbit unit + URI decomposition (alplabai/tan-cli#557, #558)
#
# Grounding, pinned Zephyr v4.4.1:
#   subsys/mgmt/hawkbit/Kconfig:29-33  HAWKBIT_POLL_INTERVAL int, "(in
#                                      minutes)", default 5, range 1 43200
#   subsys/mgmt/hawkbit/hawkbit.c:57   poll_sleep = CONFIG_HAWKBIT_POLL_INTERVAL
#                                      * SEC_PER_MIN
#   subsys/mgmt/hawkbit/Kconfig:68     HAWKBIT_SERVER "User address" (bare host)
#   subsys/mgmt/hawkbit/Kconfig:75     HAWKBIT_PORT int, default 8080
#   subsys/mgmt/hawkbit/Kconfig:168    HAWKBIT_USE_TLS depends on
#                                      NET_SOCKETS_SOCKOPT_TLS
# ---------------------------------------------------------------------

def _hawkbit_board(tmp_path: Path, url: str, poll: object = 1800) -> Path:
    poll_line = f"  poll_interval_s: {poll}\n" if poll is not None else ""
    return _write_board(tmp_path, f"""
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
    url: "{url}"
{poll_line}""")


def test_hawkbit_poll_interval_is_converted_seconds_to_minutes(
        tmp_path: Path) -> None:
    """board.yaml states SECONDS; the Kconfig symbol is MINUTES.  The schema's
    own 1800 s default is 30 min -- emitting 1800 made the fleet poll every
    30 HOURS (alplabai/tan-cli#557)."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "https://hosted.mender.io", 1800))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=30" in conf
    assert "CONFIG_HAWKBIT_POLL_INTERVAL=1800" not in conf


def test_hawkbit_poll_interval_refuses_a_non_whole_minute(tmp_path: Path) -> None:
    """90 s is 1.5 min -- not expressible in the target unit.  Refuse, naming
    the two values that are, instead of silently rounding."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "https://hosted.mender.io", 90))
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m55_hp"])
    msg = str(exc.value)
    assert "MINUTES" in msg
    assert "60" in msg and "120" in msg


def test_hawkbit_poll_interval_refuses_out_of_range(tmp_path: Path) -> None:
    """43201 min is outside Zephyr's `range 1 43200`; kconfiglib warns and
    zephyr/scripts/kconfig/kconfig.py turns that into a hard abort."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "https://hosted.mender.io", 43201 * 60))
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m55_hp"])
    assert "range 1 43200" in str(exc.value)


def test_hawkbit_https_url_is_decomposed(tmp_path: Path) -> None:
    """host -> HAWKBIT_SERVER, scheme -> port 443 + TLS.  The raw URI must
    never reach the symbol: DNS for `https://hosted.mender.io` cannot resolve
    (alplabai/tan-cli#558)."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "https://hosted.mender.io", 1800))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert 'CONFIG_HAWKBIT_SERVER="hosted.mender.io"' in conf
    assert "https://" not in conf
    assert "CONFIG_HAWKBIT_PORT=443" in conf
    # HAWKBIT_USE_TLS depends on NET_SOCKETS_SOCKOPT_TLS -- setting it alone
    # would be an unsatisfiable assignment and abort the configure.
    assert "CONFIG_NET_SOCKETS_SOCKOPT_TLS=y" in conf
    assert "CONFIG_HAWKBIT_USE_TLS=y" in conf


def test_hawkbit_http_url_keeps_plaintext_and_derives_port(tmp_path: Path) -> None:
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "http://hawkbit.lan:8080", 1800))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert 'CONFIG_HAWKBIT_SERVER="hawkbit.lan"' in conf
    assert "CONFIG_HAWKBIT_PORT=8080" in conf
    assert "CONFIG_HAWKBIT_USE_TLS" not in conf


def test_hawkbit_bare_host_placeholder_passes_through(tmp_path: Path) -> None:
    """A value with no scheme is already a bare host -- including a whole-value
    ${VAR} placeholder the build system substitutes later.  Case preserved."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "${OTA_HOST}", 1800))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert 'CONFIG_HAWKBIT_SERVER="${OTA_HOST}"' in conf
    for line in conf.splitlines():
        assert not line.startswith("CONFIG_HAWKBIT_PORT")
        assert not line.startswith("CONFIG_HAWKBIT_USE_TLS")


def test_hawkbit_url_with_a_base_path_is_refused(tmp_path: Path) -> None:
    """The DDI client builds its own request paths and has no base-path knob,
    so a path cannot be honoured -- refuse rather than drop it silently."""
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "https://hosted.mender.io/DDI", 1800))
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m55_hp"])
    assert "base path" in str(exc.value)


def test_hawkbit_url_with_unsupported_scheme_is_refused(tmp_path: Path) -> None:
    project = load_board_yaml(
        _hawkbit_board(tmp_path, "coap://hosted.mender.io", 1800))
    with pytest.raises(OrchestratorError) as exc:
        _slice_alp_conf(project, project.cores["m55_hp"])
    assert "coap" in str(exc.value)


# ---------------------------------------------------------------------
# diagnostics.modules -> the CHOICE symbol (alplabai/tan-cli#559)
#
# Zephyr's <MOD>_LOG_LEVEL int is PROMPTLESS and derived from the
# <MOD>_LOG_LEVEL_<OFF|ERR|WRN|INF|DBG> choice
# (subsys/logging/Kconfig.template.log_config), and the choice only exists
# inside the module's own `if <SYM>` block and `depends on LOG`.
# ---------------------------------------------------------------------

_DIAG_BOARD = """
som:
  sku: E1M-AEN701

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [i2c]
  m55_he:
    os: "off"

diagnostics:
  log_level: info
  modules:
    i2c: warn
    can: debug
    net_tcp: debug
    my_typo_module: debug
"""


def test_diagnostics_emits_the_choice_symbol_for_an_enabled_module(
        tmp_path: Path) -> None:
    """`i2c: warn` on a core that enables CONFIG_I2C=y emits the choice symbol
    board.schema.json documents -- never the promptless int, which Zephyr
    rejects with "is not directly user-configurable (has no prompt)"."""
    project = load_board_yaml(_write_board(tmp_path, _DIAG_BOARD))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "CONFIG_I2C=y" in conf
    assert "CONFIG_I2C_LOG_LEVEL_WRN=y" in conf
    assert "\nCONFIG_I2C_LOG_LEVEL=2" not in conf


def test_diagnostics_module_not_enabled_on_this_core_is_commented(
        tmp_path: Path) -> None:
    """`can: debug` on a core with no CONFIG_CAN=y: the choice symbol does not
    exist there, so it must be a comment, not a build-killing assignment."""
    project = load_board_yaml(_write_board(tmp_path, _DIAG_BOARD))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "# CONFIG_CAN_LOG_LEVEL_DBG=y" in conf
    for line in conf.splitlines():
        assert not line.startswith("CONFIG_CAN=y")
        assert not line.startswith("CONFIG_CAN_LOG_LEVEL")


def test_diagnostics_unknown_module_never_emits_a_live_line(
        tmp_path: Path) -> None:
    """A typo key must not abort the Zephyr configure with `attempt to assign
    the value ... to the undefined symbol MY_TYPO_MODULE_LOG_LEVEL`."""
    project = load_board_yaml(_write_board(tmp_path, _DIAG_BOARD))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "# CONFIG_MY_TYPO_MODULE_LOG_LEVEL_DBG=y" in conf
    for line in conf.splitlines():
        assert not line.startswith("CONFIG_MY_TYPO_MODULE")


def test_diagnostics_net_module_needs_the_net_log_gate(tmp_path: Path) -> None:
    """The networking log template gates its choice on NET_LOG, not LOG
    (subsys/net/Kconfig.template.log_config.net `depends on $(module-dep)`),
    so `net_tcp` must stay a comment until CONFIG_NET_LOG=y is set."""
    project = load_board_yaml(_write_board(tmp_path, _DIAG_BOARD))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    assert "# CONFIG_NET_TCP_LOG_LEVEL_DBG=y" in conf
    assert "CONFIG_NET_LOG=y" in conf.split(
        "# CONFIG_NET_TCP_LOG_LEVEL_DBG=y")[1].splitlines()[0]
    for line in conf.splitlines():
        assert not line.startswith("CONFIG_NET_TCP_LOG_LEVEL")


# ---------------------------------------------------------------------
# The guard column of `_LOG_MODULES` has to be EXACT, not approximately
# right, because a wrong guard fails SILENTLY: the Zephyr configure still
# exits 0 and only warns `The choice symbol <SYM> ... was selected (set
# =y), but no symbol ended up as the choice selection`, so the override is
# discarded with nothing to notice.  Measured against real Zephyr v4.4.1
# (`cmake -GNinja -DBOARD=native_sim -S samples/hello_world` +
# `-DEXTRA_CONF_FILE=<fragment>`):
#
#   CONFIG_LOG=y / CONFIG_MBEDTLS=y / CONFIG_MBEDTLS_LOG_LEVEL_DBG=y
#       -> EXIT=0, that warning, and the generated .config carries NO
#          CONFIG_MBEDTLS_LOG_LEVEL_DBG line at all (only
#          `# CONFIG_MBEDTLS_DEBUG is not set`)
#   ... the same fragment + CONFIG_MBEDTLS_DEBUG=y
#       -> EXIT=0, no warning, .config:493:CONFIG_MBEDTLS_LOG_LEVEL_DBG=y
# ---------------------------------------------------------------------

_MBEDTLS_DIAG_BOARD = """
som:
  sku: E1M-AEN801

libraries:
  - name: mbedtls
    cores: [m55_hp]

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [i2c]

diagnostics:
  log_level: info
  modules:
    mbedtls: debug
"""


def test_diagnostics_mbedtls_needs_the_mbedtls_debug_guard(
        tmp_path: Path) -> None:
    """`module = MBEDTLS` sits inside `if MBEDTLS_DEBUG` (modules/mbedtls/
    Kconfig:89), itself nested in `if MBEDTLS` (:31).  CONFIG_MBEDTLS=y alone
    does NOT declare the choice, so this has to stay a comment naming the real
    guard.  mbedtls is the ONE in-table module the SDK routinely enables
    (iot-fleet-ota, production-deployment, rpmsg-v2n, rpmsg-aen,
    heterogeneous-offload all emit CONFIG_MBEDTLS=y), so a guard of MBEDTLS
    alone made exactly those boards emit a silently-discarded override plus a
    Kconfig warning."""
    project = load_board_yaml(_write_board(tmp_path, _MBEDTLS_DIAG_BOARD))
    conf = _slice_alp_conf(project, project.cores["m55_hp"])
    # The two preconditions the old single-symbol guard was satisfied by:
    assert "CONFIG_MBEDTLS=y" in conf
    assert "CONFIG_LOG=y" in conf
    # ... and the override is STILL not live, because MBEDTLS_DEBUG is not set.
    for line in conf.splitlines():
        assert not line.startswith("CONFIG_MBEDTLS_LOG_LEVEL")
    assert "# CONFIG_MBEDTLS_LOG_LEVEL_DBG=y" in conf
    assert "CONFIG_MBEDTLS_DEBUG=y" in conf.split(
        "# CONFIG_MBEDTLS_LOG_LEVEL_DBG=y")[1].splitlines()[0]


def test_log_module_guard_may_be_a_multi_symbol_chain() -> None:
    """Two rows need more than one guard because their log_config template is
    nested in more than one `if`; both are pinned here so a later table edit
    cannot quietly drop the inner one.

      * mbedtls  -- `if MBEDTLS` / `if MBEDTLS_DEBUG`
                    (modules/mbedtls/Kconfig:31, :87).
      * net_ipv4 -- `if NET_IPV4` / `if NET_NATIVE_IPV4`
                    (subsys/net/ip/Kconfig.ipv4:12, :44).  NET_NATIVE_IPV4 is
                    HIDDEN -- `depends on NET_NATIVE`, `default y if NET_IPV4`
                    (subsys/net/ip/Kconfig:60-63) -- so NET_NATIVE is the
                    emittable symbol that proves it.  Measured: a fragment with
                    NET_IPV4=y + NET_LOG=y + NET_NATIVE=n reproduces the same
                    silent-discard warning; with NET_NATIVE at its default y it
                    resolves to CONFIG_NET_IPV4_LOG_LEVEL_DBG=y.
    """
    from alp_orchestrate.kconfig import _LOG_MODULES

    assert _LOG_MODULES["mbedtls"] == (
        "MBEDTLS", ("MBEDTLS", "MBEDTLS_DEBUG"), "LOG")
    assert _LOG_MODULES["net_ipv4"] == (
        "NET_IPV4", ("NET_IPV4", "NET_NATIVE"), "NET_LOG")


def test_log_module_multi_guard_goes_live_when_every_guard_is_set() -> None:
    """The chain is a guard, not a blanket refusal: with the whole chain and
    the logging gate already `=y` in the fragment the choice symbol IS
    declared, so the override must go live -- and drop the inner guard and the
    very same call must comment it out."""
    from alp_orchestrate.kconfig import _emit_diagnostics
    from alp_orchestrate.models import Slice

    class _Proj:
        diagnostics = {"modules": {"mbedtls": "debug"}}

    slice_ = Slice(core_id="m55_hp", os="zephyr")

    lines = _emit_diagnostics(
        _Proj(), slice_,
        ["CONFIG_LOG=y", "CONFIG_MBEDTLS=y", "CONFIG_MBEDTLS_DEBUG=y"])
    assert "CONFIG_MBEDTLS_LOG_LEVEL_DBG=y" in lines

    lines = _emit_diagnostics(
        _Proj(), slice_, ["CONFIG_LOG=y", "CONFIG_MBEDTLS=y"])
    assert "CONFIG_MBEDTLS_LOG_LEVEL_DBG=y" not in lines
    assert any(ln.startswith("# CONFIG_MBEDTLS_LOG_LEVEL_DBG=y")
               for ln in lines)
