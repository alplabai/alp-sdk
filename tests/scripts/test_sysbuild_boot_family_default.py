# SPDX-License-Identifier: Apache-2.0
"""A validate-clean project must not be refused by the build-plan emit.

alplabai/tan-cli#562.  `emit_sysbuild_conf` hard-defaulted `boot.method:`
to `mcuboot` for EVERY SoM family and then hard-raised on `rsa3072`,
without ever looking at the project's slices.  `buildplan._shared_artefacts`
calls it for every project, so a Yocto-only Renesas RZ/V2N project -- which
has no Zephyr slice, never runs sysbuild, and whose boot chain is the
U-Boot/FIT stack -- failed its ENTIRE build-plan emit with MCUboot-specific
advice.  `rsa3072` is exactly the value `validate._boot_signing_supported_
for_family` PERMITS for `renesas-rzv2n`, so `validate` said clean and
`build` refused: a board that passes validation could not be built at all.

Two independent halves, both pinned below:

  1. No Zephyr slice -> no sysbuild overlay.  sysbuild exists only inside
     a Zephyr build (`west build --sysbuild -- -DSB_CONF_FILE=...`).
  2. `boot.method:` defaults PER SoM FAMILY, the value board.schema.json
     has always documented ("AEN/N93 -> mcuboot, V2N/V2N-M1 -> none on the
     Zephyr slice since U-Boot owns boot on Linux") and nothing implemented.

Run locally:

    python -m pytest tests/scripts/test_sysbuild_boot_family_default.py -v
"""

from __future__ import annotations

import json
import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    OrchestratorError,
    load_board_yaml,
)
from alp_orchestrate.buildplan import emit_build_plan  # noqa: E402
from alp_orchestrate.secure import emit_sysbuild_conf  # noqa: E402

# The exact board.yaml from tan-cli#562.  `method:` is OMITTED, which is
# schema-legal; the SoM family is meant to supply it.
V2N_YOCTO_ONLY_RSA3072 = """
    som: {sku: E1M-V2N101, hw_rev: r1}
    preset: e1m-x-evk
    cores:
      a55_cluster: {os: yocto, image: alp-image-edge}
      m33_sm: {os: "off"}
    boot:
      signing:
        algorithm: rsa3072
        key_file: keys/root-rsa-3072.pem
"""

# Same SoM + same algorithm, but WITH a Zephyr slice: the sysbuild overlay
# is now a real artefact, so half 1 does not apply and half 2 has to.
V2N_WITH_ZEPHYR_SLICE_RSA3072 = """
    som: {sku: E1M-V2N101, hw_rev: r1}
    preset: e1m-x-evk
    cores:
      a55_cluster: {os: yocto, image: alp-image-edge}
      m33_sm: {os: zephyr, app: ./m33}
    boot:
      signing:
        algorithm: rsa3072
        key_file: keys/root-rsa-3072.pem
"""

# AEN, `method:` omitted: the family default must stay `mcuboot`, so the
# fix cannot be "stop emitting MCUboot".
AEN_METHOD_OMITTED = """
    som: {sku: E1M-AEN801, hw_rev: r2}
    preset: e1m-evk
    cores:
      m55_hp: {os: zephyr, app: ./src}
    boot:
      signing:
        algorithm: ecdsa_p256
        key_file: keys/dev_ecdsa_p256.pem
"""

# EXPLICIT `method: mcuboot` + rsa3072 on a family that permits rsa3072:
# the #807 refusal is still correct here and must survive -- silently
# shipping rsa2048's key length for an rsa3072-declared key is the bug
# that raise exists to prevent.  It has to be a V2N/i.MX9 board: AEN is
# the one family `validate._boot_signing_supported_for_family` rejects
# rsa3072 for outright, so an AEN board never reaches this emitter.
V2N_EXPLICIT_MCUBOOT_RSA3072 = """
    som: {sku: E1M-V2N101, hw_rev: r1}
    preset: e1m-x-evk
    cores:
      a55_cluster: {os: yocto, image: alp-image-edge}
      m33_sm: {os: zephyr, app: ./m33}
    boot:
      method: mcuboot
      signing:
        algorithm: rsa3072
        key_file: keys/root-rsa-3072.pem
"""


def _project(tmp_path: Path, body: str):
    for d in ("src", "m33"):
        (tmp_path / d).mkdir(exist_ok=True)
    path = tmp_path / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path, load_board_yaml(path)


def _plan(tmp_path: Path, board_yaml: Path, project) -> dict:
    return json.loads(emit_build_plan(project, board_yaml=board_yaml,
                                      build_root=tmp_path / "build"))


# --------------------------------------------------------------------------
# Half 1: no Zephyr slice -> no sysbuild overlay, and the build plan emits
# --------------------------------------------------------------------------

def test_yocto_only_v2n_emits_no_sysbuild_overlay(tmp_path):
    _, project = _project(tmp_path, V2N_YOCTO_ONLY_RSA3072)
    assert not any(s.os == "zephyr" for s in project.cores.values())
    assert emit_sysbuild_conf(project) == ""


def test_the_reported_board_yaml_produces_a_build_plan(tmp_path):
    """The defect itself: `tan validate` clean, `tan build` refused with
    `build.plan-unavailable`.  The whole emit used to raise here."""
    board_yaml, project = _project(tmp_path, V2N_YOCTO_ONLY_RSA3072)
    plan = _plan(tmp_path, board_yaml, project)
    assert plan["slices"], "the build plan must carry the yocto slice"
    written = [a["path"] for a in plan["sharedArtefacts"]]
    assert not any("alp_sysbuild.conf" in p for p in written), (
        "a project with no Zephyr slice must not be handed a sysbuild "
        "overlay to write")


def test_validate_clean_and_buildable_agree(tmp_path):
    """The contradiction #562 is about, asserted directly: nothing about
    this project may refuse at emit time when validation passed it."""
    from alp_orchestrate.validate import _validate_consistency

    board_yaml, project = _project(tmp_path, V2N_YOCTO_ONLY_RSA3072)
    _validate_consistency(project)          # clean -- rsa3072 is legal here
    _plan(tmp_path, board_yaml, project)    # must not raise


# --------------------------------------------------------------------------
# Half 2: boot.method defaults per SoM family
# --------------------------------------------------------------------------

def test_v2n_with_a_zephyr_slice_defaults_to_no_bootloader(tmp_path):
    """renesas-rzv2n's documented default is `none` -- U-Boot owns boot.
    Reaching the mcuboot branch is what made rsa3072 explode."""
    _, project = _project(tmp_path, V2N_WITH_ZEPHYR_SLICE_RSA3072)
    conf = emit_sysbuild_conf(project)
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=n" in conf
    assert "SB_CONFIG_BOOT_SIGNATURE_TYPE" not in conf


def test_aen_still_defaults_to_mcuboot(tmp_path):
    """alif-ensemble's documented default is `mcuboot`; the family table
    must not have flipped the default for everyone."""
    _, project = _project(tmp_path, AEN_METHOD_OMITTED)
    conf = emit_sysbuild_conf(project)
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=y" in conf
    assert "SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y" in conf


def test_explicit_mcuboot_with_rsa3072_still_refuses(tmp_path):
    """#807's refusal is untouched: an EXPLICIT `method: mcuboot` really
    does have no honest rsa3072 emit.  The fix narrows WHEN that raise is
    reached (a project that actually runs sysbuild, and opted into
    mcuboot), never whether it fires once reached."""
    _, project = _project(tmp_path, V2N_EXPLICIT_MCUBOOT_RSA3072)
    with pytest.raises(OrchestratorError, match="rsa3072"):
        emit_sysbuild_conf(project)


def test_a_project_with_no_boot_block_is_unchanged(tmp_path):
    """Absence-emits-nothing still holds, and is not what half 1 tests."""
    _, project = _project(tmp_path, """
        som: {sku: E1M-AEN801, hw_rev: r2}
        preset: e1m-evk
        cores:
          m55_hp: {os: zephyr, app: ./src}
    """)
    assert emit_sysbuild_conf(project) == ""
