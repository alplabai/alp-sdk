# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for commit e3a4c6b "bulk-strip: redundant per-core
os: from example board.yamls".

The bulk-strip removed 52 `os: zephyr` / `os: yocto` lines from 48
example board.yamls on the assumption that the orchestrator's
_resolve_topology_for_core() merges the missing field from the SoM
preset's `topology:` block.  If that merge ever regresses (someone
flips the merge precedence, drops the topology lookup, etc.), the
examples silently lose their runtime selection and the build would
go to the wrong backend.

This file locks in:

  1. For EVERY (SoM SKU × core) combination the bulk-strip touched,
     a board.yaml that omits `os:` on that core resolves to the
     expected runtime from the SoM preset's topology.

  2. Omitting the entire core entry (the customer doesn't even
     mention the core) still produces a slice with the topology's
     defaults -- app, board/machine, toolchain.

  3. An EXPLICIT `os:` override beats the topology default
     (`os: "off"` skips, `os: baremetal` flips runtime).

  4. Customer-supplied per-core fields (app:, peripherals:, …) win
     against topology while os: still resolves from topology when
     omitted -- proves the merge is per-key, not all-or-nothing.

Run locally:

    python -m pytest tests/scripts/test_topology_default_resolution.py -v
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# Per-SoM topology table (mirrors metadata/e1m_modules/E1M-<MPN>.yaml)
# ---------------------------------------------------------------------
#
# Maintained by hand because the test's whole point is to catch
# regressions where the orchestrator stops reading topology defaults.
# If a new SoM lands, append a row here AND verify the bulk-strip
# logic still applies (see commit message of e3a4c6b).
#
# Format: { sku: { core_id: expected_os } }
SOM_TOPOLOGIES: dict[str, dict[str, str]] = {
    # Alif Ensemble -- AEN3/AEN4 are M-only (no A-core); AEN5/6/7/8
    # add a Cortex-A32 cluster.
    "E1M-AEN301": {"m55_hp": "zephyr", "m55_he": "zephyr"},
    "E1M-AEN401": {"m55_hp": "zephyr", "m55_he": "zephyr"},
    "E1M-AEN501": {"a32_cluster": "yocto",
                   "m55_hp": "zephyr", "m55_he": "zephyr"},
    "E1M-AEN601": {"a32_cluster": "yocto",
                   "m55_hp": "zephyr", "m55_he": "zephyr"},
    "E1M-AEN701": {"a32_cluster": "yocto",
                   "m55_hp": "zephyr", "m55_he": "zephyr"},
    "E1M-AEN801": {"a32_cluster": "yocto",
                   "m55_hp": "zephyr", "m55_he": "zephyr"},
    # Renesas RZ/V2N -- A55 cluster + M33 system-manager.
    "E1M-V2N101": {"a55_cluster": "yocto", "m33_sm": "zephyr"},
    "E1M-V2N102": {"a55_cluster": "yocto", "m33_sm": "zephyr"},
    # V2N + DEEPX DX-M1 (same silicon as V2N, different module BOM).
    "E1M-V2M101": {"a55_cluster": "yocto", "m33_sm": "zephyr"},
    "E1M-V2M102": {"a55_cluster": "yocto", "m33_sm": "zephyr"},
    # NXP i.MX 93 -- A55 cluster + M33 (no _sm suffix).
    "E1M-NX9101": {"a55_cluster": "yocto", "m33": "zephyr"},
}


# Per-SoM: pick one M-class core to act as the "anchor" so the
# board.yaml has at least one declared entry (schema requires
# cores: with minProperties: 1).  We use the M-class core because
# every supported SoM has one (NX9101 doesn't have an M55).
_M_CORE_PER_SOM: dict[str, str] = {
    "E1M-AEN301": "m55_hp",
    "E1M-AEN401": "m55_hp",
    "E1M-AEN501": "m55_hp",
    "E1M-AEN601": "m55_hp",
    "E1M-AEN701": "m55_hp",
    "E1M-AEN801": "m55_hp",
    "E1M-V2N101": "m33_sm",
    "E1M-V2N102": "m33_sm",
    "E1M-V2M101": "m33_sm",
    "E1M-V2M102": "m33_sm",
    "E1M-NX9101": "m33",
}


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _write_board(tmp: Path, body: str) -> Path:
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


# Build the parametrize matrix: every (sku, core_id, expected_os).
_COMBOS = [
    pytest.param(sku, core, expected_os, id=f"{sku}-{core}")
    for sku, cores in SOM_TOPOLOGIES.items()
    for core, expected_os in cores.items()
]


# ---------------------------------------------------------------------
# 1. Per-(SoM, core) topology-default resolution
# ---------------------------------------------------------------------


@pytest.mark.parametrize("sku,core_id,expected_os", _COMBOS)
def test_omitted_os_resolves_to_topology_default(
    tmp_path: Path, sku: str, core_id: str, expected_os: str,
) -> None:
    """For every SoM × core that the bulk-strip touched, omitting
    `os:` on that core must resolve to the SoM topology's default."""
    anchor = _M_CORE_PER_SOM[sku]
    if core_id == anchor:
        body = f"""
som:
  sku: {sku}

cores:
  {anchor}:
    app: ./src
"""
    else:
        # Declare both the anchor (with app:) AND the core under test
        # (with app: but no os:) so we exercise the per-key merge on
        # the non-anchor core.
        body = f"""
som:
  sku: {sku}

cores:
  {anchor}:
    app: ./src_{anchor}
  {core_id}:
    app: ./src_{core_id}
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert core_id in project.cores, (
        f"core {core_id} missing from resolved project (cores: "
        f"{sorted(project.cores)})"
    )
    assert project.cores[core_id].os == expected_os, (
        f"{sku}.{core_id}: got os={project.cores[core_id].os!r}, "
        f"expected {expected_os!r}"
    )


# ---------------------------------------------------------------------
# 2. Omitted core entirely -> filled from topology
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "sku,core_id,expected_os",
    [pytest.param(sku, core, exp_os, id=f"{sku}-{core}")
     for sku, cores in SOM_TOPOLOGIES.items()
     for core, exp_os in cores.items()
     if core != _M_CORE_PER_SOM[sku]]
)
def test_undeclared_core_filled_from_topology(
    tmp_path: Path, sku: str, core_id: str, expected_os: str,
) -> None:
    """When the customer declares ONLY the anchor core, every other
    core in the SoC's cores[] list must still produce a slice -- with
    every field (os, app, machine/board, toolchain) filled from the
    SoM preset's topology block."""
    anchor = _M_CORE_PER_SOM[sku]
    body = f"""
        som:
          sku: {sku}

        cores:
          {anchor}:
            app: ./src
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    # The undeclared core must be present.
    assert core_id in project.cores
    slice_ = project.cores[core_id]
    # And carry the topology default for os.
    assert slice_.os == expected_os
    # Plus a non-empty app (topology supplies one for every core).
    assert slice_.app, (
        f"{sku}.{core_id}: undeclared core has empty app: "
        f"got {slice_.app!r}; topology must supply a default."
    )


# ---------------------------------------------------------------------
# 3. Explicit os: override beats topology
# ---------------------------------------------------------------------


def test_explicit_os_off_overrides_topology_zephyr(tmp_path: Path) -> None:
    """`os: "off"` skips a slice that topology would otherwise make
    zephyr.  Locks the override precedence on M-class cores."""
    body = """
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            app: ./src
          m55_he:
            os: "off"
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert project.cores["m55_hp"].os == "zephyr"      # topology default
    assert project.cores["m55_he"].os == "off"         # explicit override


def test_explicit_os_off_overrides_topology_yocto(tmp_path: Path) -> None:
    """`os: "off"` also skips an A-class core that topology would
    otherwise make yocto -- the override is symmetric."""
    body = """
        som:
          sku: E1M-V2N101

        cores:
          a55_cluster:
            os: "off"
          m33_sm:
            app: ./src
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert project.cores["a55_cluster"].os == "off"
    assert project.cores["m33_sm"].os == "zephyr"      # topology default


def test_explicit_os_baremetal_overrides_topology_zephyr(tmp_path: Path) -> None:
    """`os: baremetal` on an M-class core overrides the topology's
    zephyr default -- the rare hand-written-firmware path."""
    body = """
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            os: baremetal
            app: ./src
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    assert project.cores["m55_hp"].os == "baremetal"


# ---------------------------------------------------------------------
# 3b. The OS runtime is class-derived and NOT cross-overridable (issue #95).
# A Cortex-A core runs Yocto/Linux; a Cortex-M core runs Zephyr/RTOS. A
# board.yaml may disable a core (`off`) or drop it to `baremetal`, but it
# must NOT select the *other* class's OS. baremetal stays allowed.
# ---------------------------------------------------------------------


def test_cross_class_zephyr_on_a_core_rejected(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-V2N101

        cores:
          a55_cluster:
            os: zephyr
            app: ./linux
          m33_sm:
            app: ./m33
    """
    with pytest.raises(OrchestratorError, match="not selectable|runtime"):
        load_board_yaml(_write_board(tmp_path, body))


def test_cross_class_yocto_on_m_core_rejected(tmp_path: Path) -> None:
    body = """
        som:
          sku: E1M-V2N101

        cores:
          a55_cluster:
            app: ./linux
            image: alp-image-edge
          m33_sm:
            os: yocto
            app: ./m33
    """
    with pytest.raises(OrchestratorError, match="not selectable|runtime"):
        load_board_yaml(_write_board(tmp_path, body))


def test_class_matching_explicit_os_still_allowed(tmp_path: Path) -> None:
    # an explicit os that MATCHES the core class is fine (redundant, not a
    # cross-class override) -- this is what shipped board.yamls/tests use.
    body = """
        som:
          sku: E1M-V2N101

        cores:
          a55_cluster:
            os: yocto
            app: ./linux
          m33_sm:
            os: zephyr
            app: ./m33
    """
    project = load_board_yaml(_write_board(tmp_path, body))
    assert project.cores["a55_cluster"].os == "yocto"
    assert project.cores["m33_sm"].os == "zephyr"


# ---------------------------------------------------------------------
# 4. Per-key merge (customer overrides app:, topology fills os:)
# ---------------------------------------------------------------------


def test_per_key_merge_app_overrides_topology(tmp_path: Path) -> None:
    """The customer's `app:` wins over the topology's default app
    while os: still resolves from topology.  Proves the merge is
    per-key, not all-or-nothing -- the case that supports the
    bulk-strip pattern of `app: ./src` with no `os:` line."""
    body = """
        som:
          sku: E1M-V2N101

        cores:
          m33_sm:
            app: ./my_custom_dir
            peripherals: [adc]
            libraries: [cmsis_dsp]
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    slice_ = project.cores["m33_sm"]
    # Customer-supplied:
    assert slice_.app == "./my_custom_dir"
    assert slice_.peripherals == ["adc"]
    assert slice_.libraries == ["cmsis_dsp"]
    # Topology-supplied:
    assert slice_.os == "zephyr"
    assert slice_.board == "alp_e1m_v2n101_m33_sm"
    assert slice_.toolchain == "arm-zephyr-eabi"


def test_per_key_merge_yocto_a_cluster(tmp_path: Path) -> None:
    """Symmetric proof on the Yocto side: customer's app: + image:
    win, topology supplies os: + machine: + toolchain."""
    body = """
        som:
          sku: E1M-V2N101

        cores:
          a55_cluster:
            app: ./linux
            image: alp-image-edge
          m33_sm:
            app: ./m33
    """
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    a55 = project.cores["a55_cluster"]
    assert a55.app == "./linux"
    assert a55.image == "alp-image-edge"
    # Topology-supplied:
    assert a55.os == "yocto"
    assert a55.machine == "e1m-v2n101-a55"
    assert a55.toolchain == "poky-glibc"


# ---------------------------------------------------------------------
# 5. Sanity: shipped examples still produce expected per-core os
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "example_path,expected",
    [
        # Single-OS AEN examples -- post-strip, just `app: ./src` on m55_hp.
        ("examples/audio/audio-loopback/board.yaml",
         {"m55_hp": "zephyr", "m55_he": "zephyr"}),
        ("examples/peripheral-io/i2c-scanner/board.yaml",
         {"m55_hp": "zephyr", "m55_he": "zephyr"}),
        # Heterogeneous-offload: both `os:` lines stripped; both
        # should resolve from topology.
        ("examples/multicore/heterogeneous-offload/board.yaml",
         {"a55_cluster": "yocto", "m33_sm": "zephyr"}),
        # rpmsg-imx93: both lines stripped on the NX9 m33 (no _sm).
        ("examples/multicore/rpmsg-imx93/board.yaml",
         {"a55_cluster": "yocto", "m33": "zephyr"}),
        # rpmsg-aen: A32 + M55-HP both stripped.
        ("examples/multicore/rpmsg-aen/board.yaml",
         {"a32_cluster": "yocto", "m55_hp": "zephyr"}),
        # rpmsg-v2n: A55 + M33-SM stripped.
        ("examples/multicore/rpmsg-v2n/board.yaml",
         {"a55_cluster": "yocto", "m33_sm": "zephyr"}),
        # ai-object-detection-realtime: A55 explicit off + M33-SM stripped.
        ("examples/camera-vision/ai-object-detection-realtime/board.yaml",
         {"a55_cluster": "off", "m33_sm": "zephyr"}),
    ],
)
def test_shipped_examples_resolve_expected_os(
    example_path: str, expected: dict[str, str],
) -> None:
    """Load the actual example board.yamls the bulk-strip touched and
    confirm each declared core resolves to the expected runtime.
    This is the proof that the cleanup is observably safe end-to-end."""
    path = REPO / example_path
    project = load_board_yaml(path)
    for core_id, want_os in expected.items():
        assert core_id in project.cores, (
            f"{example_path}: core {core_id} missing"
        )
        assert project.cores[core_id].os == want_os, (
            f"{example_path}.{core_id}: got "
            f"{project.cores[core_id].os!r}, want {want_os!r}"
        )
