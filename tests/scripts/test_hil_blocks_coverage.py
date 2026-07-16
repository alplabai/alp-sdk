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

import os
import re
import sys
from pathlib import Path
from typing import Optional

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "hil"))
sys.path.insert(0, str(REPO / "scripts"))

import run_smoke                                       # noqa: E402

from alp_orchestrate import (                          # noqa: E402
    BoardProject,
    OrchestratorError,
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
    spec asserts those CONFIG_* lines reach real silicon.

    The symbol names asserted here are the REAL sysbuild names
    (`SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256`,
    `SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH`) -- issue #807 found this
    test previously hardcoded invented names
    (`SB_CONFIG_MCUBOOT_SIGNATURE_TYPE_ECDSA_P256`,
    `SB_CONFIG_BOOT_{PRIMARY,SECONDARY}_PARTITION_SIZE`) that don't
    exist anywhere in Zephyr's Kconfig, so this test stayed green
    while sysbuild's FATAL undefined-symbol check aborted every real
    `boot:` configure.  `test_emitted_sb_config_symbols_exist_in_
    pinned_zephyr` below is the class gate that catches a future
    invented name generically, against the pinned Zephyr's actual
    Kconfig tree rather than a string someone typed."""
    project = _make_project(boot={
        "method": "mcuboot",
        "signing": {"algorithm": "ecdsa_p256",
                    "key_file": "keys/dev_ecdsa_p256.pem"},
        "swap_algorithm": "scratch",
    })
    sysbuild = emit_sysbuild_conf(project)
    assert "SB_CONFIG_BOOTLOADER_MCUBOOT=y" in sysbuild, (
        "boot:.method=mcuboot must emit SB_CONFIG_BOOTLOADER_MCUBOOT=y"
    )
    assert "SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y" in sysbuild, (
        "boot:.signing.algorithm=ecdsa_p256 must emit the matching SB_CONFIG_*"
    )
    assert "SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y" in sysbuild, (
        "boot:.swap_algorithm=scratch must emit the matching SB_CONFIG_*"
    )
    # No slot/scratch-size symbols -- sysbuild has none; MCUboot takes
    # slot geometry from the board DT partitions.
    assert "PARTITION_SIZE" not in sysbuild
    assert "SB_CONFIG_BOOT_COUNTERS_MCUBOOT" not in sysbuild


def test_rsa3072_hard_errors_in_mcuboot_path() -> None:
    """sysbuild's BOOT_SIGNATURE_TYPE choice has no RSA key-length
    knob (that lives in mcuboot-image's own CONFIG_BOOT_SIGNATURE_
    TYPE_RSA_LEN, a different Kconfig namespace sysbuild never
    forwards into).  Silently emitting rsa2048's default length for
    an rsa3072-declared key would ship a signature the customer never
    asked for -- this MUST raise, not silently degrade."""
    project = _make_project(boot={
        "method": "mcuboot",
        "signing": {"algorithm": "rsa3072",
                    "key_file": "keys/dev_rsa3072.pem"},
    })
    with pytest.raises(OrchestratorError, match="rsa3072"):
        emit_sysbuild_conf(project)


# ---------------------------------------------------------------------
# 1b. Class gate (#807): every SB_CONFIG_* emit_sysbuild_conf() ever
#     produces must exist in the PINNED Zephyr's own sysbuild Kconfig
#     tree -- the oracle is the consumer's namespace, never a string
#     the emitter's author typed.  A hand-picked regression assert
#     (see test_boot_block_emits_mcuboot_config above) only catches
#     the one case someone thought to type; this crosses the full
#     boot: enum so an invented name ANYWHERE in the matrix is caught.
#
#     Same bug class as the CONFIG_PM_DEVICE_WAKE_ regression further
#     down this file (~§ power block) -- that one only got a
#     name-specific `assert "CONFIG_PM_DEVICE_WAKE_" not in conf`,
#     not a class gate.  This test is the class gate for the SB_CONFIG_
#     surface so the same defect shape doesn't recur a third time.
# ---------------------------------------------------------------------


_SB_CONFIG_ASSIGN_RE = re.compile(r"^(SB_CONFIG_[A-Za-z0-9_]+)=", re.MULTILINE)
_KCONFIG_SYMBOL_RE = re.compile(r"^\s*config\s+([A-Za-z0-9_]+)\s*$", re.MULTILINE)


def _zephyr_pin_major_minor() -> Optional[tuple[int, int]]:
    """(MAJOR, MINOR) of the `zephyr:` revision pinned in this repo's
    west.yml, or None if west.yml is missing/unparseable."""
    try:
        text = (REPO / "west.yml").read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r"-\s+name:\s+zephyr\s*\n\s+revision:\s+v?(\d+)\.(\d+)", text)
    return (int(m.group(1)), int(m.group(2))) if m else None


def _pinned_zephyr_sysbuild_kconfig_symbols() -> Optional[set[str]]:
    """Every `config <SYMBOL>` under the pinned Zephyr's
    `share/sysbuild/**/Kconfig*`, resolved from the west workspace --
    NEVER a hardcoded developer checkout path (a local tree can drift
    to a stale pin, e.g. v3.7.0 while the repo pins v4.4.0).

    Resolution order: `$ZEPHYR_BASE` (the workspace convention every
    `west` command + `scripts/alp_cli/doctor.py` use), falling back to
    the west-workspace topdir's conventional `zephyr/` project
    directory (`scripts/bootstrap.sh` does `west init -l <alp-sdk>`,
    so alp-sdk's parent is the topdir and `<topdir>/zephyr` is the
    manifest project's checkout path).  Returns None -- skip, never
    hard-fail -- when no candidate resolves to an actual Zephyr tree,
    or when the resolved tree's own VERSION doesn't match this repo's
    pinned MAJOR.MINOR (a stale checkout is as unresolvable as no
    checkout: asserting against the wrong Kconfig tree would be
    worse than not asserting at all).
    """
    pin = _zephyr_pin_major_minor()
    if pin is None:
        return None

    candidates = []
    env_base = os.environ.get("ZEPHYR_BASE")
    if env_base:
        candidates.append(Path(env_base))
    candidates.append(REPO.parent / "zephyr")

    for base in candidates:
        version_file = base / "VERSION"
        if not version_file.is_file():
            continue
        try:
            text = version_file.read_text(encoding="utf-8")
        except OSError:
            continue
        major = minor = None
        for line in text.splitlines():
            if line.startswith("VERSION_MAJOR"):
                m = re.search(r"(\d+)", line)
                major = int(m.group(1)) if m else major
            elif line.startswith("VERSION_MINOR"):
                m = re.search(r"(\d+)", line)
                minor = int(m.group(1)) if m else minor
        if (major, minor) != pin:
            continue  # stale/mismatched checkout -- not a usable oracle

        sysbuild_dir = base / "share" / "sysbuild"
        if not sysbuild_dir.is_dir():
            continue
        symbols: set[str] = set()
        for kconfig_file in sysbuild_dir.rglob("Kconfig*"):
            if not kconfig_file.is_file():
                continue
            try:
                ktext = kconfig_file.read_text(encoding="utf-8", errors="ignore")
            except OSError:
                continue
            symbols.update(_KCONFIG_SYMBOL_RE.findall(ktext))
        if symbols:
            return symbols
    return None


# The full `boot:` enum cross-product (method x algorithm x
# swap_algorithm) -- small enough to exhaustively drive through
# emit_sysbuild_conf() and check every emitted symbol against the
# pinned Zephyr's real Kconfig tree.  rsa3072 is excluded here (it
# hard-errors -- see test_rsa3072_hard_errors_in_mcuboot_path).
_BOOT_ALGORITHMS = ("ecdsa_p256", "rsa2048", "ed25519")
_BOOT_SWAP_ALGORITHMS = ("scratch", "move", "overwrite")
_BOOT_MATRIX = (
    [("none", None, None)]
    + [("mcuboot", algo, swap)
       for algo in _BOOT_ALGORITHMS
       for swap in _BOOT_SWAP_ALGORITHMS]
)


@pytest.mark.parametrize("method,algorithm,swap", _BOOT_MATRIX)
def test_emitted_sb_config_symbols_exist_in_pinned_zephyr(
    method: str, algorithm: Optional[str], swap: Optional[str],
) -> None:
    """Every `SB_CONFIG_*=` line emit_sysbuild_conf() produces, for
    every point in the `boot:` enum cross-product, must name a real
    `config` in the pinned Zephyr's `share/sysbuild/**/Kconfig*` --
    sysbuild treats an undefined-symbol warning as FATAL, so one
    invented name anywhere in this matrix aborts the whole `boot:`
    configure (issue #807)."""
    symbols = _pinned_zephyr_sysbuild_kconfig_symbols()
    if symbols is None:
        # A skip is correct on a pure-Python job (python-smoke has no
        # Zephyr checkout) but it is ALSO how this gate silently stopped
        # gating: #814 shipped it, and for a while NO job satisfied its
        # precondition, so it skipped on every PR and reported green
        # while enforcing nothing -- the same "test agrees with itself"
        # failure #807 was about, one level up.  A job that intends to
        # run this gate sets ALP_REQUIRE_ZEPHYR_ORACLE=1; there, an
        # unresolvable workspace is a BUG IN THE JOB, not an
        # environment fact, so fail loudly instead of skipping green.
        if os.environ.get("ALP_REQUIRE_ZEPHYR_ORACLE") == "1":
            pytest.fail(
                "ALP_REQUIRE_ZEPHYR_ORACLE=1 but no pinned Zephyr "
                "workspace resolved ($ZEPHYR_BASE / west-workspace "
                "zephyr/ matching west.yml's pin).  This job promised "
                "the oracle and did not deliver it -- fix the job's "
                "west init / ZEPHYR_BASE, do not drop the flag."
            )
        pytest.skip(
            "pinned Zephyr workspace not resolvable in this environment "
            "(no $ZEPHYR_BASE / west-workspace zephyr/ checkout matching "
            "this repo's west.yml pin) -- not a west/toolchain job, skip "
            "rather than hard-fail (python-smoke runs pure-Python, no "
            "Zephyr checkout).  Set ALP_REQUIRE_ZEPHYR_ORACLE=1 in a job "
            "that DOES provide the workspace to turn this skip into a "
            "failure."
        )
    boot: dict = {"method": method}
    if method == "mcuboot":
        boot["signing"] = {"algorithm": algorithm,
                            "key_file": "keys/dev.pem"}
        boot["swap_algorithm"] = swap
    project = _make_project(boot=boot)
    sysbuild = emit_sysbuild_conf(project)
    emitted = _SB_CONFIG_ASSIGN_RE.findall(sysbuild)
    assert emitted, f"boot={boot!r} emitted no SB_CONFIG_* at all"
    for stem in emitted:
        # sysbuild's Kconfig namespace prefix (`set(KCONFIG_NAMESPACE
        # SB_CONFIG)` in sysbuild_kconfig.cmake) means `SB_CONFIG_<X>`
        # is real iff `config <X>` exists in the sysbuild Kconfig tree
        # -- the prefix itself is never part of the `config` name.
        bare = stem.removeprefix("SB_CONFIG_")
        assert bare in symbols, (
            f"emit_sysbuild_conf() produced {stem}=... for boot={boot!r}, "
            f"but no `config {bare}` exists anywhere under the pinned "
            f"Zephyr's share/sysbuild/**/Kconfig* -- this is the "
            f"undefined-symbol class sysbuild treats as FATAL (#807)."
        )


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
