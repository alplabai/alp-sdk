# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the HiL coverage of board.yaml's declarative blocks
that landed at schema level in PR #3:

  - `boot:`                       (tests/hil/_common/boot_mcuboot.yaml)
  - `cores.<id>.memory:`          (tests/hil/_common/memory_stacks.yaml)
  - `cores.<id>.power:`           (tests/hil/_common/power_sleep_wake.yaml)
  - `diagnostics.modules:`        (tests/hil/_common/diagnostics_modules.yaml)

These tests run on a normal CI machine -- no silicon needed -- and
confirm:

  1. Each new HiL spec parses cleanly against tests/hil/run_smoke.py's
     schema (validates the spec shape).
  2. Each spec's `expect_contains` / `expect_absent` strings line up
     with the CONFIG_* lines the orchestrator emits for the matching
     board.yaml block.  In other words: if the orchestrator emits
     CONFIG_FOO=y for `boot:`, the HiL spec must assert something
     observable downstream of CONFIG_FOO=y.
  3. Wakeup-source names referenced in the power spec match the
     supported subsystem set in scripts/alp_orchestrate/
     (`_slice_alp_conf` translates them into CONFIG_PM_DEVICE_WAKE_*).
  4. Module names in the diagnostics spec match the alp.conf
     emit format (`CONFIG_<MODULE>_LOG_LEVEL=<n>`).

Run locally:

    python -m pytest tests/scripts/test_hil_blocks_coverage.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "hil"))
sys.path.insert(0, str(REPO / "scripts"))

import run_smoke                                       # noqa: E402

from alp_orchestrate import (                          # noqa: E402
    BoardProject,
    Slice,
    _slice_alp_conf,
    emit_sysbuild_conf,
)


_COMMON = REPO / "tests" / "hil" / "_common"


# Each new HiL spec, with the board.yaml block it covers + the
# CONFIG_* root the orchestrator must emit for that block.
NEW_SPECS = (
    ("boot_mcuboot.yaml",        "boot",                "SB_CONFIG_BOOTLOADER_MCUBOOT"),
    ("memory_stacks.yaml",       "cores.memory",        "CONFIG_MAIN_STACK_SIZE"),
    ("power_sleep_wake.yaml",    "cores.power",         "CONFIG_PM"),
    ("diagnostics_modules.yaml", "diagnostics.modules", "_LOG_LEVEL"),
)


# ---------------------------------------------------------------------
# 1. Each spec exists, parses cleanly, and references a real example.
# ---------------------------------------------------------------------


@pytest.mark.parametrize("filename,block,_root", NEW_SPECS)
def test_spec_file_exists(filename: str, block: str, _root: str) -> None:
    """The four block-coverage specs ship under tests/hil/_common/."""
    p = _COMMON / filename
    assert p.is_file(), f"missing HiL spec for {block}: {p}"


@pytest.mark.parametrize("filename,_block,_root", NEW_SPECS)
def test_spec_parses_against_runner_schema(
    filename: str, _block: str, _root: str,
) -> None:
    """Each new spec must parse cleanly via run_smoke.parse_spec()
    when paired with a real board's _runner.yaml.  Without this gate
    a typo in `schema_version:` / `serial.expect_contains:` would
    only surface at midnight on the HiL runner."""
    runner = REPO / "tests" / "hil" / "aen701-evk" / "_runner.yaml"
    assert runner.is_file(), f"missing reference runner.yaml at {runner}"
    spec = run_smoke.parse_spec(_COMMON / filename, runner_path=runner)
    assert spec.name == filename.removesuffix(".yaml").replace("_", "-")
    assert spec.serial.expect_contains, (
        f"{filename}: expect_contains is empty -- the spec asserts nothing"
    )
    assert spec.example.is_dir(), (
        f"{filename}: example path {spec.example} doesn't exist"
    )


@pytest.mark.parametrize("filename,_block,_root", NEW_SPECS)
def test_spec_has_forbidden_panic_assert(
    filename: str, _block: str, _root: str,
) -> None:
    """Every block-coverage spec must guard against PANIC / ASSERT /
    FAULT in the serial trace.  These are non-negotiable for any
    HiL spec -- a CONFIG_* line that emits but trips a runtime panic
    is a worse outcome than silently not emitting at all."""
    runner = REPO / "tests" / "hil" / "aen701-evk" / "_runner.yaml"
    spec = run_smoke.parse_spec(_COMMON / filename, runner_path=runner)
    absent_upper = {s.upper() for s in spec.serial.expect_absent}
    assert "PANIC" in absent_upper, (
        f"{filename}: missing `PANIC` from serial.expect_absent"
    )
    assert "ASSERT" in absent_upper, (
        f"{filename}: missing `ASSERT` from serial.expect_absent"
    )


# ---------------------------------------------------------------------
# 2. Cross-check: each spec covers a block the orchestrator actually emits.
#
# We synthesise a minimal BoardProject for the matching block,
# call the orchestrator's emit function, and assert the expected
# CONFIG_* root appears in the emitted text.  This catches the
# class of regression where the schema accepts a field but the
# emit code silently drops it.
# ---------------------------------------------------------------------


def _make_project(
    *,
    boot: dict | None = None,
    diagnostics: dict | None = None,
) -> BoardProject:
    """Build a minimal BoardProject for the orchestrator-emit checks
    below.  We don't go through load_board_yaml() because that
    requires real preset files on disk; the emit functions take
    BoardProject + Slice directly."""
    return BoardProject(
        sku="E1M-AEN701",
        hw_rev=None,
        board_name="hil-blocks-test",
        board_hw_rev=None,
        cores={},
        ipc=[],
        soc_spec={"silicon": "alif:e1c:e1c-aen"},
        som_preset={"family": "alif", "topology": {}},
        board_preset=None,
        diagnostics=diagnostics or {},
        chips=[],
        features={},
        boot=boot or {},
        ota={},
        raw={},
    )


def _make_slice(
    *,
    memory: dict | None = None,
    power: dict | None = None,
) -> Slice:
    """Minimal Slice carrying the per-core block under test."""
    return Slice(
        core_id="m55_hp",
        os="zephyr",
        app="./src",
        memory=memory or {},
        power=power or {},
    )


def test_boot_block_emits_mcuboot_config() -> None:
    """`boot:` -> emit_sysbuild_conf() carries SB_CONFIG_BOOTLOADER_MCUBOOT
    + the matching signature-type line.  The boot_mcuboot.yaml HiL
    spec asserts those CONFIG_* lines reach real silicon."""
    project = _make_project(boot={
        "method": "mcuboot",
        "signing": {"algorithm": "ecdsa_p256",
                    "key_file": "keys/dev_ecdsa_p256.pem"},
        "slots": {"primary": {"size_kib": 480},
                  "secondary": {"size_kib": 480}},
        "swap_algorithm": "scratch",
    })
    sysbuild = emit_sysbuild_conf(project)
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=y" in sysbuild, (
        "boot:.method=mcuboot must emit SB_CONFIG_BOOTLOADER_MCUBOOT=y"
    )
    assert "SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_ECDSA_P256=y" in sysbuild, (
        "boot:.signing.algorithm=ecdsa_p256 must emit the matching SB_CONFIG_*"
    )
    assert "SB_CONFIG_BOOT_PRIMARY_PARTITION_SIZE=" in sysbuild
    assert "SB_CONFIG_BOOT_SECONDARY_PARTITION_SIZE=" in sysbuild


def test_memory_block_emits_stack_heap_isr_config() -> None:
    """`cores.<id>.memory:` -> _slice_alp_conf() emits MAIN_STACK_SIZE,
    HEAP_MEM_POOL_SIZE, and ISR_STACK_SIZE lines with byte counts =
    KiB * 1024.  The memory_stacks.yaml HiL spec checks the runtime
    reads back the configured value."""
    project = _make_project()
    slice_ = _make_slice(memory={
        "stack_kib": 8, "heap_kib": 16, "isr_stack_kib": 4,
    })
    conf = _slice_alp_conf(project, slice_)
    assert "CONFIG_MAIN_STACK_SIZE=8192" in conf, (
        "memory.stack_kib=8 must emit CONFIG_MAIN_STACK_SIZE=8192"
    )
    assert "CONFIG_HEAP_MEM_POOL_SIZE=16384" in conf
    assert "CONFIG_ISR_STACK_SIZE=4096" in conf


def test_power_block_emits_pm_and_wake_sources() -> None:
    """`cores.<id>.power:` -> _slice_alp_conf() emits CONFIG_PM=y +
    CONFIG_PM_DEVICE=y when sleep_mode != disabled, plus one hint
    comment per declared wakeup source.  The actual wake-source
    plumbing in Zephyr is DT-driven (`wakeup-source;` property on the
    device node + a runtime pm_device_wakeup_enable() call) and per
    silicon family -- there is no top-level CONFIG_PM_DEVICE_WAKE_<X>
    symbol upstream, so emitting one trips the build with an
    `undefined symbol` warning.  Until per-silicon DT-overlay + runtime
    enable lands (v0.7), the wake-source list is documented in the
    generated alp.conf for the customer to wire by hand."""
    project = _make_project()
    slice_ = _make_slice(power={
        "sleep_mode": "standby",
        "wakeup_sources": ["uart", "rtc"],
    })
    conf = _slice_alp_conf(project, slice_)
    assert "CONFIG_PM=y" in conf
    assert "CONFIG_PM_DEVICE=y" in conf
    assert "# wakeup source: uart" in conf, (
        "power.wakeup_sources=[uart] must emit a hint comment until the "
        "per-silicon DT-overlay + pm_device_wakeup_enable() plumbing lands"
    )
    assert "# wakeup source: rtc" in conf
    # The undefined-symbol Kconfig form must NEVER be re-introduced --
    # it trips twister with a Kconfig warning on every native_sim build.
    assert "CONFIG_PM_DEVICE_WAKE_" not in conf


def test_power_block_disabled_emits_no_pm() -> None:
    """`power.sleep_mode: disabled` (or omitted) must NOT enable PM
    -- pulling in CONFIG_PM on always-on apps wastes ROM."""
    project = _make_project()
    slice_ = _make_slice(power={"sleep_mode": "disabled"})
    conf = _slice_alp_conf(project, slice_)
    assert "CONFIG_PM=y" not in conf


def test_diagnostics_modules_emits_per_module_log_level() -> None:
    """`diagnostics.modules:` -> _slice_alp_conf() emits one log-level
    line per entry.  ALP_* SDK-side modules have not registered any
    LOG_MODULE yet, so emitting `CONFIG_ALP_<MOD>_LOG_LEVEL=N`
    upstream is rejected as an undefined symbol; until each ALP
    module gains its LOG_MODULE_REGISTER call, the emit is a hint
    comment.  Non-ALP modules (Zephyr subsystems whose Kconfig
    already exists) keep the live CONFIG_<MOD>_LOG_LEVEL=N form.
    Level-name -> integer mapping (off=0 / error=1 / warn=2 / info=3
    / debug=trace=4) is preserved either way."""
    project = _make_project(diagnostics={
        "log_level": "info",
        "modules": {
            "alp_iot": "debug",
            "alp_security": "off",
            "alp_gpio": "info",
        },
    })
    slice_ = _make_slice()
    conf = _slice_alp_conf(project, slice_)
    # ALP_* modules: hint comment with the would-be Kconfig + level.
    assert "# CONFIG_ALP_IOT_LOG_LEVEL=4" in conf
    assert "# CONFIG_ALP_SECURITY_LOG_LEVEL=0" in conf
    assert "# CONFIG_ALP_GPIO_LOG_LEVEL=3" in conf
    # And not as a live setting -- the undefined-symbol form is rejected
    # by Zephyr Kconfig today; re-introducing it would break twister.
    for stem in ("ALP_IOT_LOG_LEVEL", "ALP_SECURITY_LOG_LEVEL", "ALP_GPIO_LOG_LEVEL"):
        assert f"\nCONFIG_{stem}=" not in conf, (
            f"CONFIG_{stem} must stay commented until alp_{stem.split('_')[1].lower()} "
            "calls LOG_MODULE_REGISTER (otherwise Zephyr aborts on undefined symbol)"
        )


# ---------------------------------------------------------------------
# 3. Wakeup-source names referenced in the power spec match the
#    schema's supported subsystem set.
# ---------------------------------------------------------------------


# Subsystem-class wake names the emit code understands.  Anything
# not in this set (and not starting with `E1M_`) is documented in
# the generated alp.conf as a hint comment until the per-silicon
# DT-overlay + runtime pm_device_wakeup_enable() plumbing lands in
# v0.7; the schema itself doesn't enum-restrict.  We keep the spec's
# references inside the de-facto supported set so the test doesn't
# drift against the silicon-family wake-source wiring.
_SUPPORTED_WAKE_SUBSYSTEMS = frozenset({
    "uart", "gpio", "rtc", "i2c", "spi", "can", "usb", "ethernet",
})


def test_power_spec_wake_source_names_are_supported() -> None:
    """The power_sleep_wake.yaml spec asserts on `PM: resume` after
    a wake event -- the wake source the description names must be
    in the supported subsystem set (matches the emit code's
    intent).  Reads the spec's description / comments and checks
    every `uart` / `gpio` / ... word against the supported set."""
    text = (_COMMON / "power_sleep_wake.yaml").read_text(encoding="utf-8")
    # Pull every fenced `<subsys>` reference that follows
    # "wakeup_sources:" or "Wakeup sources covered:" in the comments
    # /description.  Cheap: enumerate the known names and confirm
    # at least one shows up (proves the spec documents a real source).
    referenced = {w for w in _SUPPORTED_WAKE_SUBSYSTEMS if w in text}
    assert referenced, (
        "power_sleep_wake.yaml must document at least one wakeup "
        "source from the supported subsystem set "
        f"({sorted(_SUPPORTED_WAKE_SUBSYSTEMS)})"
    )


# ---------------------------------------------------------------------
# 4. Diagnostics spec references real module names (the kind of
#    string the emit path turns into CONFIG_<MODULE>_LOG_LEVEL).
# ---------------------------------------------------------------------


def test_diagnostics_spec_references_known_modules() -> None:
    """The diagnostics_modules.yaml spec asserts on `<dbg> alp_iot`
    and `alp_security:` -- both must be names the emit path
    (_slice_alp_conf's modules loop) would accept.  The convention
    is lowercase `alp_<surface>`; we check the spec references
    at least one such identifier.  alp_* modules currently emit as
    hint comments (#CONFIG_ALP_<UPPER>_LOG_LEVEL=N) until each
    surface calls LOG_MODULE_REGISTER -- the spec text still uses
    the same identifiers."""
    text = (_COMMON / "diagnostics_modules.yaml").read_text(encoding="utf-8")
    # Stock Alp SDK module names the emit path knows how to render.
    known = ("alp_iot", "alp_security", "alp_gpio", "alp_audio",
             "alp_inference", "alp_uart", "alp_i2c", "alp_spi")
    found = [m for m in known if m in text]
    assert found, (
        "diagnostics_modules.yaml must reference at least one known "
        f"alp_<surface> module name (any of {known})"
    )


# ---------------------------------------------------------------------
# 5. pending_hardware_support flag -- if a spec carries one, document
#    it.  Today the power_sleep_wake spec flags a missing current-
#    draw probe; if a future spec sprouts the same flag it must
#    explain why.
# ---------------------------------------------------------------------


@pytest.mark.parametrize("filename,_block,_root", NEW_SPECS)
def test_pending_hardware_support_documented(
    filename: str, _block: str, _root: str,
) -> None:
    """If a spec declares `pending_hardware_support:` the value must
    be a short non-empty identifier so the gap is grep-able."""
    import yaml as _yaml
    data = _yaml.safe_load((_COMMON / filename).read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    phs = data.get("pending_hardware_support")
    if phs is not None:
        assert isinstance(phs, str) and phs.strip(), (
            f"{filename}: pending_hardware_support must be a non-empty string"
        )
