# SPDX-License-Identifier: Apache-2.0
"""
Shared fixtures for the tests/scripts/test_orchestrate_*.py suite -- the
test_orchestrate_*.py suite split (issue #460 / #673 Phase 3,
module-size reduction).

Not a test module itself: no test_* functions/classes live here, just
the constants + helpers every split-out test_orchestrate_*.py file needs
to invoke scripts/alp_orchestrate/ the same way the monolith did.
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _synthetic_nx9101_root(tmp_path: Path, monkeypatch) -> Path:
    """Build a scratch metadata root carrying a synthetic E1M-NX9101
    preset, isolated from the repo's real
    `metadata/e1m_modules/imx93/hw-revisions.yaml`, and monkeypatch
    `alp_orchestrate.METADATA_ROOT`/`BOARD_SCHEMA` onto it. Returns the
    scratch metadata root (also pass it as `load_board_yaml(...,
    metadata_root=...)`'s own argument -- the module patch alone isn't
    enough because `load_board_yaml` takes `metadata_root` as an
    explicit parameter).

    Why: #1025's hw_rev-buildable gate refuses the REAL E1M-NX9101
    outright (its only hw_rev, imx93 r1, is `status: tbd`) before
    `load_board_yaml` ever reaches SoM-preset-specific logic
    (mailbox-controller-TBD carve-out blocking, wifi_ble-TBD IoT
    fallback, ...) that several tests need to exercise in isolation,
    and there is no second, buildable NX9101 hw_rev to pick instead.
    `family_revision_buildable` on a MISSING hw-revisions.yaml table
    returns None (unknown), not False, so a scratch root with no
    `e1m_modules/imx93/hw-revisions.yaml` at all makes the #1025 gate
    a no-op here -- this is test isolation of the SoM-preset logic, not
    a claim about the real E1M-NX9101's buildability (which stays
    refused everywhere else in the suite)."""
    import shutil

    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "nxp" / "imx9"
    schemas = meta / "schemas"
    for d in (e1m, socs, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "nxp" / "imx9" / "imx93.json",
                socs / "imx93.json")

    preset = e1m / "E1M-NX9101.yaml"
    preset.write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-NX9101
        family: nxp-imx9
        silicon: nxp:imx9:imx93
        on_module:
          wifi_ble: TBD
        topology:
          a55_cluster:
            os: yocto
            app: alp-image-edge
            machine: e1m-nx9101-a55
            toolchain: poky-glibc
          m33:
            os: zephyr
            app: alp-stock-shim
            board: alp_e1m_nx9101_m33
            toolchain: arm-zephyr-eabi
        mailbox:
          controller: TBD
          channels:
            - { id: 0, reserved_for: alp_default_rpmsg }
            - { id: 1, reserved_for: app }
        default_hw_rev: r1
    """).lstrip("\n"), encoding="utf-8")

    import alp_orchestrate
    monkeypatch.setattr(alp_orchestrate, "METADATA_ROOT", meta)
    monkeypatch.setattr(alp_orchestrate, "BOARD_SCHEMA",
                        schemas / "board.schema.json")
    return meta


V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

libraries:
  - name: mbedtls
    cores: [a55_cluster]
  - name: nlohmann-json
    cores: [a55_cluster]
  - name: cmsis-dsp
    cores: [m33_sm]

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""
