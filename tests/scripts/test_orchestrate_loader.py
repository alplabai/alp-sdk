# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- board.yaml loading + topology
resolution (load_board_yaml()).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_loader.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import V2N_HAPPY, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    BoardProject,
    OrchestratorError,
    load_board_yaml,
)


V2N_TOPOLOGY_FALLBACK = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


V1_REJECT = """
som:
  sku: E1M-V2N101

os: zephyr

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""


UNKNOWN_CORE = """
som:
  sku: E1M-V2N101

cores:
  m99_garbage:
    os: zephyr
    app: ./garbage
"""


# ---------------------------------------------------------------------
# 1. load_board_yaml -- happy path
# ---------------------------------------------------------------------


def test_load_board_yaml_v2n_happy(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_HAPPY)
    project = load_board_yaml(path)
    assert isinstance(project, BoardProject)
    assert project.sku == "E1M-V2N101"
    assert project.hw_rev == "r1"
    assert project.board_name == "test-v2n-board"
    assert set(project.cores.keys()) == {"a55_cluster", "m33_sm"}

    a55 = project.cores["a55_cluster"]
    m33 = project.cores["m33_sm"]
    assert a55.os == "yocto"
    assert a55.app == "./linux"               # customer override wins
    assert a55.image == "alp-image-edge"
    assert a55.machine == "e1m-v2n101-a55"    # inherited from SoM topology
    assert a55.toolchain == "poky-glibc"
    assert a55.peripherals == ["ethernet", "usb"]

    assert m33.os == "zephyr"
    assert m33.app == "./m33"
    assert m33.board == "alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33"      # inherited
    assert m33.toolchain == "arm-zephyr-eabi"
    assert m33.peripherals == ["adc", "pwm", "i2c", "gpio"]
    assert m33.libraries == ["cmsis-dsp"]

    assert len(project.ipc) == 1
    assert project.ipc[0].name == "alp_default_rpmsg"
    assert project.ipc[0].carve_out_kb == 512


# ---------------------------------------------------------------------
# 2. Loader topology fallback
# ---------------------------------------------------------------------


def test_load_board_yaml_topology_fallback(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V2N_TOPOLOGY_FALLBACK)
    project = load_board_yaml(path)

    # The a55_cluster wasn't declared, so it picks up the V2N101
    # topology default: os: yocto + app: alp-image-edge.
    a55 = project.cores["a55_cluster"]
    assert a55.os == "yocto"
    assert a55.app == "alp-image-edge"
    assert a55.image is None
    assert a55.machine == "e1m-v2n101-a55"

    m33 = project.cores["m33_sm"]
    assert m33.os == "zephyr"
    assert m33.app == "./m33"


# ---------------------------------------------------------------------
# 3. Loader rejects v1 top-level `os:`
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_v1_os(tmp_path: Path) -> None:
    path = _write_board(tmp_path, V1_REJECT)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    # The schema says `not: required: [os]`, so the message should
    # mention the violation.  We accept any clear failure.
    assert "schema" in str(excinfo.value).lower() or \
           "os" in str(excinfo.value).lower()


# ---------------------------------------------------------------------
# 4. Loader rejects unknown core id
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_unknown_core(tmp_path: Path) -> None:
    path = _write_board(tmp_path, UNKNOWN_CORE)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m99_garbage" in msg
    # Phase B gap fix G-4: error now points at the SoM SKU's
    # `topology:` (the customer-actionable surface) and offers a
    # "did you mean" hint listing the preset's actual core keys.
    assert "topology" in msg
    assert "did you mean" in msg.lower()


def test_load_board_yaml_rejects_duplicate_top_level_key(tmp_path: Path) -> None:
    """#1127: a repeated `som:` key must FAIL the load, not silently keep
    only the last value (`yaml.safe_load` does that with no error)."""
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
som:
  sku: E1M-V2N101
cores:
  m55_hp:
    os: zephyr
    app: ./src
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "duplicate key" in str(excinfo.value).lower()


def test_load_board_yaml_rejects_unknown_features_key(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features:
  ipc:
    framing: nanopb
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "features" in msg
    assert "ipc" in msg


def test_load_board_yaml_rejects_empty_features_block(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-AEN801
cores:
  m55_hp:
    os: zephyr
    app: ./src
features: {}
""")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "features" in str(excinfo.value)


def test_load_board_yaml_rejects_board_preset_family_mismatch(tmp_path: Path) -> None:
    path = _write_board(tmp_path, """
        som:
          sku: E1M-V2N101

        preset: e1m-evk

        cores:
          m33_sm:
            app: ./src
    """)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    assert "hosts SoM families" in str(excinfo.value)


# ---------------------------------------------------------------------
# 4b. Phase B gap fix G-4: cross-class `som.sku:` swap diagnostic
# ---------------------------------------------------------------------


# Customer kept the AEN-shaped `cores.m55_hp:` block but swapped
# `som.sku:` to E1M-V2N101 (E1M-X topology: m33_sm + a55_cluster, no
# m55_hp).  Pre-fix the orchestrator silently dropped the m55_hp
# entry; the customer got an empty slice with no diagnostic.
#
# Was E1M-NX9101 (an in-family, Cortex-M-class SoM with a genuinely
# different topology shape from AEN's m55_hp/m55_he) until #1025:
# NX9101's only hw_rev (imx93 r1) is `status: tbd`, so
# `load_board_yaml` now refuses it outright (SdkRevisionNotBuildable)
# before this test's cores:/topology: mismatch is ever reached --
# there is no second hw_rev to pick instead. E1M-V2N101 (E1M-X family)
# is the nearest buildable SoM with a topology that also has no
# `m55_hp` key, so it still exercises the same "wrong-shaped cores:"
# hard-fail; swap back to E1M-NX9101 once imx93 r1 carries a buildable
# status, if an in-family repro is preferred.
G4_CROSS_CLASS_SWAP = """
som:
  sku: E1M-V2N101

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [i2c]
"""


# Customer remembered to rename one core (m33_sm) but forgot the
# second (m55_hp).  Pre-#603 this only soft-WARNed and silently
# dropped the `m55_hp` slice while the file still validated "clean";
# #603 makes this a hard error like the all-unmatched case above --
# there is no compatibility policy that tolerates an unknown core key.
# See G4_CROSS_CLASS_SWAP's comment above for why this is E1M-V2N101,
# not E1M-NX9101 (#1025).
G4_PARTIAL_MATCH = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33_sm
    peripherals: [i2c]
  m55_hp:
    os: zephyr
    app: ./m55_hp
    peripherals: [spi]
"""


def test_unknown_cores_key_raises(tmp_path: Path) -> None:
    """G-4 hard-fail: NO `cores:` key matches `topology:`."""
    path = _write_board(tmp_path, G4_CROSS_CLASS_SWAP)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "did you mean" in msg.lower()
    assert "m33" in msg
    assert "a55_cluster" in msg
    assert "E1M-V2N101" in msg
    assert "topology" in msg


def test_partial_match_raises(tmp_path: Path) -> None:
    """#603: a PARTIAL `cores:` mismatch (one valid key, one typo) is
    now also a hard error -- it must NOT silently drop the typo'd
    slice and report the file as clean."""
    path = _write_board(tmp_path, G4_PARTIAL_MATCH)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "did you mean" in msg.lower()
    assert "m33" in msg
    assert "E1M-V2N101" in msg


# ---------------------------------------------------------------------
# 4c. #1088: `cacheable: true` on a `kind: rpmsg` entry has no
#     cache-maintenance implementation behind it -- reject at load time
# ---------------------------------------------------------------------


def test_load_board_yaml_rejects_rpmsg_cacheable_true(tmp_path: Path) -> None:
    """`cfg->cacheable` is stored on the rpc backend struct
    (src/backends/rpc/{zephyr,yocto}_drv.c) and never read again -- no
    `sys_cache_*` call exists anywhere under src/ or include/.  A
    `cacheable: true` rpmsg entry would therefore select a code path
    that promises coherency it can't deliver, which is worse than no
    flag at all, so the loader refuses it outright rather than
    silently honouring it."""
    body = V2N_HAPPY.replace(
        "    name: alp_default_rpmsg\n",
        "    name: alp_default_rpmsg\n    cacheable: true\n",
    )
    assert "cacheable: true" in body  # guard against a silent no-op replace
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "alp_default_rpmsg" in msg
    assert "rpmsg" in msg
    assert "cacheable: true" in msg
    assert "1088" in msg


# ---------------------------------------------------------------------
# 5. _silicon_to_soc_path -- migrated onto resolve_soc_path() (issue #1004)
# ---------------------------------------------------------------------


def test_silicon_to_soc_path_rejects_malformed_ref(tmp_path: Path) -> None:
    """Pins the OrchestratorError shape #1004's migration promised to
    preserve: a malformed `silicon:` ref still raises OrchestratorError
    with this exact message, not resolve_soc_path()'s bare `None`."""
    from alp_orchestrate.loader import _silicon_to_soc_path

    with pytest.raises(OrchestratorError) as excinfo:
        _silicon_to_soc_path("acme:widget", tmp_path)
    assert str(excinfo.value) == "silicon ref 'acme:widget' is not a triple-colon string"




# ---------------------------------------------------------------------
# 6. The wrong-board SW-DP IDR preflight pair (#1355)
#
# `debug.expect_dpidr` + `debug.jlink_device[<core>]` are ONE guard, and a
# downstream flasher (tan `flash_plan.py::validate_flow_d_preflight_args`)
# refuses a half-armed pair at plan time rather than skipping the check --
# so the loader must never hand out one without the other, and must refuse
# loudly when a core that genuinely flashes has lost its attach profile.
# ---------------------------------------------------------------------


def _e8_debug(**overrides):
    """A resolved E8 `debug:` block, shaped exactly like the real
    AE822FA0E5597LS0 one in metadata/socs/alif/ensemble/e8.json."""
    block = {
        "pyocd_target": "AE822FA0E5597LS0",
        "jlink_device": {"m55_hp": "Cortex-M55", "m55_he": "Cortex-M55"},
        "jlink_flash_device": "AE822FA0E5597LS0_M55_HE",
        "expect_dpidr": "0x4C013477",
    }
    block.update(overrides)
    return block


def test_resolve_flow_d_preflight_returns_the_measured_pair() -> None:
    from alp_orchestrate.loader import _resolve_flow_d_preflight

    assert _resolve_flow_d_preflight(_e8_debug(), "m55_hp") == \
        ("0x4C013477", "Cortex-M55")


def test_resolve_flow_d_preflight_drops_both_when_core_has_no_profile(
) -> None:
    """`debug.jlink_device` is legitimately sparse: the E8's a32_cluster is
    a Linux A-cluster, not a J-Link flash target. Returning `expect_dpidr`
    alone for it would be the half-armed shape a flasher refuses."""
    from alp_orchestrate.loader import _resolve_flow_d_preflight

    assert _resolve_flow_d_preflight(_e8_debug(), "a32_cluster") == \
        (None, None)


def test_resolve_flow_d_preflight_drops_device_when_dpidr_unmeasured(
) -> None:
    """E1M-AEN701's shape: an attach profile, no measured DPIDR. The
    attach profile must NOT be emitted on its own -- unarmed is safe,
    half-armed is a hard refusal."""
    from alp_orchestrate.loader import _resolve_flow_d_preflight

    debug = _e8_debug()
    del debug["expect_dpidr"]
    assert _resolve_flow_d_preflight(debug, "m55_hp") == (None, None)


def test_flow_d_preflight_pair_refuses_a_flashing_core_with_no_profile(
) -> None:
    """The real metadata gap this guard exists for: `expect_dpidr` is
    published, the slice DOES take Flow D (zephyr + a part-number flash
    profile), and its core lost its attach profile -- so the pair silently
    collapsed to nothing and the write would proceed unguarded. Refuse,
    naming the core and the file to fix."""
    from alp_orchestrate.loader import _enforce_flow_d_preflight_pair
    from alp_orchestrate.models import Slice

    debug = _e8_debug()
    del debug["jlink_device"]["m55_hp"]
    slice_ = Slice(
        core_id="m55_hp",
        os="zephyr",
        jlink_flash_device=debug["jlink_flash_device"],
        expect_dpidr=None,
        jlink_device=None,
    )
    with pytest.raises(OrchestratorError) as excinfo:
        _enforce_flow_d_preflight_pair(slice_, debug, "E1M-AEN801")
    msg = str(excinfo.value)
    assert "m55_hp" in msg
    assert "expect_dpidr" in msg
    assert "jlink_device" in msg


def test_flow_d_preflight_pair_allows_an_unmeasured_variant() -> None:
    """The CONVERSE must stay legal: no `expect_dpidr` at all is the state
    of every variant nobody has measured, and inventing one is worse than
    leaving the guard unarmed."""
    from alp_orchestrate.loader import _enforce_flow_d_preflight_pair
    from alp_orchestrate.models import Slice

    debug = _e8_debug()
    del debug["expect_dpidr"]
    del debug["jlink_device"]["m55_hp"]
    slice_ = Slice(core_id="m55_hp", os="zephyr",
                   jlink_flash_device=debug["jlink_flash_device"])
    _enforce_flow_d_preflight_pair(slice_, debug, "E1M-AEN701")  # no raise


def test_flow_d_preflight_pair_ignores_a_non_flow_d_slice() -> None:
    """An A-core (or any slice whose variant publishes no part-number
    flash profile) is not a Flow D target: it emits no `flash_args` a
    preflight could half-arm, so a missing attach profile there is not a
    gap. Checking it would fail every AEN project on `a32_cluster`."""
    from alp_orchestrate.loader import _enforce_flow_d_preflight_pair
    from alp_orchestrate.models import Slice

    debug = _e8_debug()
    for slice_ in (
        Slice(core_id="a32_cluster", os="yocto",
              jlink_flash_device=debug["jlink_flash_device"]),
        Slice(core_id="m33_sm", os="zephyr", jlink_flash_device=None),
    ):
        _enforce_flow_d_preflight_pair(slice_, debug, "E1M-AEN801")  # no raise


def test_load_board_yaml_aen801_carries_the_pair_on_both_m55s(
    tmp_path: Path,
) -> None:
    """End-to-end through the real metadata: the resolved Slices, not just
    the helpers, carry both halves."""
    path = _write_board(tmp_path, "som:\n  sku: E1M-AEN801\ncores:\n  a32_cluster:\n    os: \"off\"\n")
    project = load_board_yaml(path)
    for core_id in ("m55_hp", "m55_he"):
        slice_ = project.cores[core_id]
        assert slice_.expect_dpidr == "0x4C013477"
        assert slice_.jlink_device == "Cortex-M55"
    a32 = project.cores["a32_cluster"]
    assert a32.expect_dpidr is None
    assert a32.jlink_device is None


# ---------------------------------------------------------------------
# `_resolve_slot0_load_address` (tan-cli#353) -- unit-level, independent
# of whether any given SoC JSON currently arms `jlink_flash_device` (only
# `_resolve_load_board_yaml`'s caller gates on that; the resolver itself
# must be right for every SoM shape, including ones -- E1M-AEN401,
# E1M-AEN601 -- whose SoC JSON does not arm Flow D *today*).
# ---------------------------------------------------------------------


def test_resolve_slot0_load_address_no_override_defaults_for_hp_role() -> None:
    """A SoM preset with NO `memory_map:` override at all (the shape of
    every E1M-AEN401/E1M-AEN601 preset -- both declare `m55_hp` AND
    `m55_he` in `topology:`, but only the `m55_hp` Zephyr board tree is
    generated today, #999) must still resolve the stock default for the
    `hp` role, not just `he` -- the stock symmetric layout has one slot0
    window shared by whichever core boots it."""
    from alp_orchestrate.loader import _resolve_slot0_load_address

    assert _resolve_slot0_load_address({}, "m55_hp") == "0x80010000"
    assert _resolve_slot0_load_address({"memory_map": []}, "m55_hp") == \
        "0x80010000"


def test_resolve_slot0_load_address_no_override_defaults_for_he_role() -> None:
    from alp_orchestrate.loader import _resolve_slot0_load_address

    assert _resolve_slot0_load_address({}, "m55_he") == "0x80010000"


def test_resolve_slot0_load_address_disjoint_override_per_role() -> None:
    """E1M-AEN801's #1069 disjoint-slot0 shape: each role reads its OWN
    declared region, not the stock default."""
    from alp_orchestrate.loader import _resolve_slot0_load_address

    preset = {"memory_map": [
        {"name": "he_slot0", "base": 0x80010000,
         "accessible_from": ["m55_he"]},
        {"name": "hp_slot0", "base": 0x802B0000,
         "accessible_from": ["m55_hp"]},
    ]}
    assert _resolve_slot0_load_address(preset, "m55_he") == "0x80010000"
    assert _resolve_slot0_load_address(preset, "m55_hp") == "0x802b0000"


def test_resolve_slot0_load_address_half_authored_override_raises() -> None:
    """A `memory_map:` that declares a disjoint slot0 window for ONE role
    but not its sibling is a half-authored map: `gen_zephyr_board.py`'s
    `_aen_role_slot0_map` refuses to build a board for the undeclared
    role (falling back to the stock default there would silently land it
    on top of the sibling's declared window, #1069's exact bug), so the
    manifest resolver must refuse too, not silently invent a value no
    board was ever generated for."""
    from alp_orchestrate.loader import _resolve_slot0_load_address
    from alp_orchestrate.models import OrchestratorError

    preset = {"memory_map": [
        {"name": "hp_slot0", "base": 0x802B0000,
         "accessible_from": ["m55_hp"]},
    ]}
    with pytest.raises(OrchestratorError):
        _resolve_slot0_load_address(preset, "m55_he")


def test_resolve_slot0_load_address_wrong_accessible_from_raises() -> None:
    """A declared `<role>_slot0` region that is NOT exclusively
    `accessible_from` its own role's core is a misdeclared disjoint
    window (the whole point of #1069's fix is per-core exclusivity);
    `_aen_role_slot0_map` raises rather than accept it, and so must
    this resolver."""
    from alp_orchestrate.loader import _resolve_slot0_load_address
    from alp_orchestrate.models import OrchestratorError

    preset = {"memory_map": [
        {"name": "he_slot0", "base": 0x80200000,
         "accessible_from": ["m55_he", "m55_hp"]},
    ]}
    with pytest.raises(OrchestratorError):
        _resolve_slot0_load_address(preset, "m55_he")


def test_resolve_slot0_load_address_default_derives_from_gen_zephyr_board(
) -> None:
    """The no-override default must be COMPUTED from
    `gen_zephyr_board`'s own `_AEN_MRAM_BASE`/`_AEN_MCUBOOT_KIB`, not a
    locally pinned literal only a test keeps in sync -- change either
    constant and this resolver's default must move with it, with no
    edit here."""
    import gen_zephyr_board
    from alp_orchestrate.loader import _resolve_slot0_load_address

    original_kib = gen_zephyr_board._AEN_MCUBOOT_KIB
    try:
        gen_zephyr_board._AEN_MCUBOOT_KIB = 128
        assert _resolve_slot0_load_address({}, "m55_he") == "0x80020000"
    finally:
        gen_zephyr_board._AEN_MCUBOOT_KIB = original_kib
    # Restored: back to the real, unmodified constant's value.
    assert _resolve_slot0_load_address({}, "m55_he") == "0x80010000"


# ---------------------------------------------------------------------
# `_enforce_slot0_disjoint_across_roles` (#1384) -- the both-roles-
# collision guard: a dual-M55 AEN SoM whose m55_he and m55_hp slices
# resolve `flash_args.slot0_load_address` to the SAME address is the
# #1069 HE/HP MRAM collision, expressed in flash_args instead of only
# in board generation.
# ---------------------------------------------------------------------


def test_enforce_slot0_disjoint_across_roles_refuses_a_collision() -> None:
    from alp_orchestrate.loader import _enforce_slot0_disjoint_across_roles
    from alp_orchestrate.models import OrchestratorError, Slice

    cores = {
        "m55_he": Slice(core_id="m55_he", os="zephyr",
                         slot0_load_address="0x80010000"),
        "m55_hp": Slice(core_id="m55_hp", os="zephyr",
                         slot0_load_address="0x80010000"),
    }
    with pytest.raises(OrchestratorError) as excinfo:
        _enforce_slot0_disjoint_across_roles(cores, "E1M-AEN401")
    msg = str(excinfo.value)
    assert "m55_he" in msg
    assert "m55_hp" in msg
    assert "0x80010000" in msg


def test_enforce_slot0_disjoint_across_roles_allows_a_disjoint_pair() -> None:
    """#1069's actual fix (E1M-AEN801's declared `he_slot0`/`hp_slot0`
    override) must stay legal."""
    from alp_orchestrate.loader import _enforce_slot0_disjoint_across_roles
    from alp_orchestrate.models import Slice

    cores = {
        "m55_he": Slice(core_id="m55_he", os="zephyr",
                         slot0_load_address="0x80010000"),
        "m55_hp": Slice(core_id="m55_hp", os="zephyr",
                         slot0_load_address="0x802b0000"),
    }
    _enforce_slot0_disjoint_across_roles(cores, "E1M-AEN801")  # no raise


def test_enforce_slot0_disjoint_across_roles_ignores_a_single_m55_core(
) -> None:
    """A SoM with only one M55 slice resolved (or neither slice carrying
    a slot0 address at all -- `jlink_flash_device` absent) has nothing
    to compare; must not raise on a missing sibling."""
    from alp_orchestrate.loader import _enforce_slot0_disjoint_across_roles
    from alp_orchestrate.models import Slice

    cores = {
        "m55_hp": Slice(core_id="m55_hp", os="zephyr",
                         slot0_load_address="0x80010000"),
    }
    _enforce_slot0_disjoint_across_roles(cores, "E1M-AEN401")  # no raise

    cores_no_flow_d = {
        "m55_he": Slice(core_id="m55_he", os="zephyr",
                         slot0_load_address=None),
        "m55_hp": Slice(core_id="m55_hp", os="zephyr",
                         slot0_load_address=None),
    }
    _enforce_slot0_disjoint_across_roles(
        cores_no_flow_d, "E1M-AEN401")  # no raise


def test_enforce_slot0_disjoint_across_roles_ignores_a_parked_sibling(
) -> None:
    """#1295: E1M-AEN301's `power-managed-sensor` example parks `m55_hp`
    with `os: "off"` and runs only `m55_he` as Zephyr. Once E3's variant
    started publishing `debug.jlink_flash_device` (#1295), both roles
    resolve the SAME no-override default `slot0_load_address` -- but
    `m55_hp` is parked, so it is never a flash target and this must NOT
    raise: comparing a live core's slot0 against a parked core's moot one
    is not the #1069 hazard this guard exists to catch."""
    from alp_orchestrate.loader import _enforce_slot0_disjoint_across_roles
    from alp_orchestrate.models import Slice

    cores = {
        "m55_he": Slice(core_id="m55_he", os="zephyr",
                         slot0_load_address="0x80010000"),
        "m55_hp": Slice(core_id="m55_hp", os="off",
                         slot0_load_address="0x80010000"),
    }
    _enforce_slot0_disjoint_across_roles(cores, "E1M-AEN301")  # no raise
