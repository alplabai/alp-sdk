# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/west_commands/runners/alif_flash.py's pure
reset-vector -> ATOC-shape selection logic (bench-proven 2026-07-19:
app-write-mram over the SE-UART burns BOTH the MRAM slot0-XIP shape and
the ITCM-load shape, so this runner AUTO-DETECTS the shape from the
app binary's own reset vector instead of rejecting --mram-xip).

The real module does ``from runners.core import RunnerCaps,
ZephyrBinaryRunner`` -- Zephyr's runner base, only importable from an
active west/Zephyr workspace. This suite stubs that one external
symbol in ``sys.modules`` (never installed on a bare Windows/pytest
host) so the REAL alif_flash module imports cleanly and its pure
functions run unmodified, same "stub the boundary, exercise the real
code" shape as tests/scripts/test_flash_backends.py.

Run locally:

    py -3.14 -m pytest tests/scripts/test_alif_flash_runner.py -v
"""

from __future__ import annotations

import json
import logging
import sys
import types
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))

# --- stub the one external dependency (Zephyr's runners.core) ----------
if "runners.core" not in sys.modules:
    _fake_core = types.ModuleType("runners.core")

    class _RunnerCaps:
        def __init__(self, **kw):
            self.__dict__.update(kw)

    class _ZephyrBinaryRunner:
        def __init__(self, cfg):
            self.cfg = cfg
            # do_run integration tests (below) exercise the #2262 ATOC
            # guard end to end; they need a check_call that records
            # rather than actually executing (so a real app-gen-toc /
            # app-write-mram is never required) and a logger the real
            # do_run/`_run_atoc_guard` can call unconditionally.
            self.calls = []
            self.logger = logging.getLogger("test_alif_flash_runner")

        def check_call(self, cmd, cwd=None):
            self.calls.append((list(cmd), cwd))

    _fake_core.RunnerCaps = _RunnerCaps
    _fake_core.ZephyrBinaryRunner = _ZephyrBinaryRunner
    sys.modules["runners.core"] = _fake_core

from runners import alif_flash  # noqa: E402


# ---------------------------------------------------------------------
# _cpu_suffix -- --device -> HE/HP
# ---------------------------------------------------------------------


def test_cpu_suffix_defaults_to_he_when_device_unset() -> None:
    assert alif_flash._cpu_suffix(None) == "HE"
    assert alif_flash._cpu_suffix("") == "HE"


def test_cpu_suffix_reads_he_and_hp_suffix() -> None:
    assert alif_flash._cpu_suffix("AE822FA0E5597LS0_HE") == "HE"
    assert alif_flash._cpu_suffix("AE822FA0E5597LS0_HP") == "HP"
    # case-insensitive: a real m55_hp --device string signs as HP, not
    # silently falling through to the HE default (the bug this task fixes).
    assert alif_flash._cpu_suffix("m55_hp") == "HP"


# ---------------------------------------------------------------------
# _reset_vector -- raw zephyr.bin bytes -> u32
# ---------------------------------------------------------------------


def _bin_with_reset_vector(rv: int, sp: int = 0x20080000) -> bytes:
    return sp.to_bytes(4, "little") + rv.to_bytes(4, "little") + b"\x00" * 8


def test_reset_vector_reads_le_u32_at_offset_4(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(_bin_with_reset_vector(0x80011F15))
    assert alif_flash._reset_vector(p) == 0x80011F15


def test_reset_vector_rejects_truncated_file(tmp_path) -> None:
    p = tmp_path / "zephyr.bin"
    p.write_bytes(b"\x00\x01\x02")  # < 8 bytes, no vector table
    with pytest.raises(RuntimeError, match="too small"):
        alif_flash._reset_vector(p)


# ---------------------------------------------------------------------
# _select_app_shape -- the auto-detect matrix
# ---------------------------------------------------------------------


def test_select_app_shape_slot0_mram_xip_for_8001_reset_vector() -> None:
    # bench-proven reset vector after boot: 0x80011F15
    shape = alif_flash._select_app_shape(0x80011F15, "HE")
    assert shape == {
        "cpu_id": "M55_HE",
        "mramAddress": "0x80010000",
        "flags": ["boot"],
    }
    assert "loadAddress" not in shape


def test_select_app_shape_itcm_he() -> None:
    shape = alif_flash._select_app_shape(0x58000401, "HE")
    assert shape == {
        "cpu_id": "M55_HE",
        "loadAddress": "0x58000000",
        "flags": ["load", "boot"],
    }
    assert "mramAddress" not in shape


def test_select_app_shape_itcm_hp_uses_hp_cpu_id_and_load_address() -> None:
    # The vector's own top byte (0x50) is the core, not --device: an
    # HP-range vector must emit loadAddress 0x50000000 / cpu_id M55_HP,
    # NOT the HE address (the bug this task fixes: "today an m55_hp
    # flash signs as M55_HE"). --device agrees here so no mismatch.
    shape = alif_flash._select_app_shape(0x50000401, "HP")
    assert shape == {
        "cpu_id": "M55_HP",
        "loadAddress": "0x50000000",
        "flags": ["load", "boot"],
    }
    assert shape["loadAddress"] != "0x58000000"


def test_select_app_shape_itcm_vector_device_mismatch_raises() -> None:
    # An HE-linked (0x58xxxxxx) vector but --device says HP: the vector
    # is the ground truth for TCM, so this must raise, not silently pick
    # one side (defense-in-depth against a wrong/absent --device).
    with pytest.raises(RuntimeError, match="M55-HE.*M55-HP"):
        alif_flash._select_app_shape(0x58000401, "HP")
    with pytest.raises(RuntimeError, match="M55-HP.*M55-HE"):
        alif_flash._select_app_shape(0x50000401, "HE")


def test_select_app_shape_unrecognised_reset_vector_raises() -> None:
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x00000123, "HE")


def test_select_app_shape_mram_base_below_slot0_offset_raises() -> None:
    # 0x8000xxxx is MRAM base, below the 0x10000 slot0 load offset -- the
    # fault case (an un-relocated/garbage-linked image), must NOT be
    # accepted as slot0.
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x80000000, "HE")


def test_select_app_shape_system_mram_base_excluded_from_slot0() -> None:
    # 0x80580000 is System MRAM base, the slot0 region's exclusive upper
    # edge -- pinned OUT of the accepted slot0 range.
    with pytest.raises(RuntimeError, match="unrecognised reset vector"):
        alif_flash._select_app_shape(0x80580000, "HE")


def test_select_app_shape_high_in_window_vector_accepted() -> None:
    # Near the TOP of HE's own disjoint slot0 window (#1069:
    # 0x80010000..0x802b0000), not just the narrow 0x8001xxxx band --
    # still HE's mramAddress (the window base), not the raw vector.
    shape = alif_flash._select_app_shape(0x802affff, "HE")
    assert shape["cpu_id"] == "M55_HE"
    assert shape["mramAddress"] == "0x80010000"
    assert shape["flags"] == ["boot"]


def test_select_app_shape_large_slot0_vector_accepted() -> None:
    # A slot0 vector well above the narrow 0x8001xxxx band (e.g. a large
    # app whose entry point lands past the old 64KiB-only mask) --
    # inside HP's own window (#1069: 0x802b0000..0x80550000).
    shape = alif_flash._select_app_shape(0x80300000, "HP")
    assert shape["cpu_id"] == "M55_HP"
    assert shape["mramAddress"] == "0x802b0000"
    assert shape["flags"] == ["boot"]


def test_select_app_shape_vector_in_sibling_window_raises() -> None:
    # #1069 core of the fix: a vector that resolves inside the OTHER
    # core's disjoint slot0 window must be refused, not silently
    # flashed under --device's cpu_id (that refusal IS the guard --
    # this is exactly the pre-#1069 collision shape, just now caught
    # before app-gen-toc instead of after both cores overwrite MRAM).
    with pytest.raises(RuntimeError, match="M55_HE slot0 window"):
        alif_flash._select_app_shape(0x80010000, "HP")
    with pytest.raises(RuntimeError, match="M55_HP slot0 window"):
        alif_flash._select_app_shape(0x802b0000, "HE")


def test_select_app_shape_vector_in_a32_window_margin_raises() -> None:
    # scripts/aen_atoc.SLOT0_WINDOWS['A32_0'] (0x80002000..0x80578000)
    # covers a margin above M55_HP's ceiling (0x80550000) that belongs to
    # neither M55 window. Regression guard: this Zephyr-only runner must
    # still refuse a vector landing in that margin, not silently mis-map
    # it onto M55_HP's window base -- this is the gap the else-branch's
    # old 'HE'-or-else-'HP' ternary left open the moment SLOT0_WINDOWS
    # grew a third key. #1981 review: A32_0 is now excluded from this
    # runner's window lookup entirely (it never stages an A32 image), so
    # the refusal is the plain "outside every declared slot0 window"
    # message, not an A32-specific one that used to mislabel a genuine
    # M55 mis-link as "staged by a different flash path".
    with pytest.raises(RuntimeError, match="outside every declared slot0 window"):
        alif_flash._select_app_shape(0x80560000, "HP")


def test_select_app_shape_vector_at_he_window_returns_unchanged_address() -> None:
    # HE keeps the pre-#1069 address unchanged (#1069 decided layout).
    shape = alif_flash._select_app_shape(0x80011F15, "HE")
    assert shape["mramAddress"] == "0x80010000"


def test_select_app_shape_vector_at_hp_window_returns_moved_address() -> None:
    # HP moved off the old shared 0x80010000 window onto the ex-OTA
    # slot1 window (#1069 decided layout).
    shape = alif_flash._select_app_shape(0x802b0801, "HP")
    assert shape["mramAddress"] == "0x802b0000"


# ---------------------------------------------------------------------
# _build_atoc_config -- config shape reflected into the ATOC JSON
# ---------------------------------------------------------------------


def test_build_atoc_config_itcm_shape_matches_bench_recipe() -> None:
    shape = alif_flash._select_app_shape(0x58000401, "HE")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"binary": "myapp.bin"' in cfg
    assert '"loadAddress": "0x58000000"' in cfg
    assert '"mramAddress"' not in cfg
    assert '"flags": ["load", "boot"]' in cfg
    assert '"cpu_id": "M55_HE"' in cfg
    assert '"ALP-HE"' in cfg


def test_build_atoc_config_slot0_shape_omits_load_flag() -> None:
    shape = alif_flash._select_app_shape(0x80011F15, "HE")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"mramAddress": "0x80010000"' in cfg
    assert '"loadAddress"' not in cfg
    assert '"flags": ["boot"]' in cfg
    assert '"load"' not in cfg


def test_build_atoc_config_hp_uses_alp_hp_section() -> None:
    shape = alif_flash._select_app_shape(0x50000401, "HP")
    cfg = alif_flash._build_atoc_config("myapp", shape)
    assert '"ALP-HP"' in cfg
    assert '"cpu_id": "M55_HP"' in cfg


# ---------------------------------------------------------------------
# do_run's #2262 pre-burn ATOC guard -- end-to-end against the real
# do_run/_run_atoc_guard, with only the SE-UART subprocess boundary
# (alif_flash._run_maintenance) and check_call (see the fake
# ZephyrBinaryRunner above) stubbed. This exercises the actual wiring:
# _atoc_section_name -> allowed set, the guard running strictly before
# app-write-mram, the verdict JSON, and --replace-atoc.
# ---------------------------------------------------------------------

_CLEAN_HE_GETTOC = (
    "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
    "      312 |  0.5.0| u V  |\n"
    "|   ALP-HE | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |"
    "     4480 |  1.0.0| uLVB |\n"
)

_FOREIGN_GETTOC = (
    "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
    "      312 |  0.5.0| u V  |\n"
    "| BOOTLOAD | A32_0  | 0x80002000 | 0x8057A8F0 | ---------- | 0x80002000 |"
    "    28813 |  0.4.3| u VB |\n"
    "|   A32_APP | A32_0  | 0x80020000 | 0x8057B2F0 | ---------- | ---------- |"
    "  2290048 |  1.0.0| u V  |\n"
    "|   HP_APP | M55-HP | 0x8057D230 | 0x8057C830 | 0x50000000 | 0x50000000 |"
    "     4480 |  1.0.0| uLVB |\n"
    "|   HE_APP | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |"
    "     4480 |  1.0.0| uLVB |\n"
)

_NO_ATOC_TEXT = "No ATOC found on target device.\n"
_COMPLIANT_BANNER = "SES A1 v1.110.0 Mar  4 2026 19:06:23\n"

# A pre-provisioned module's factory ATOC (HIGH-2 review, #2262):
# `zephyr/sysbuild/aen/README.md`'s "SoM-maker provisioning model" --
# `| MCUBOOT- | M55-HE | ... | uLVB |`.
_FACTORY_MCUBOOT_GETTOC = (
    "|   DEVICE |  CM0+  | 0x8057C6F0 | 0x8057BCF0 | ---------- | ---------- |"
    "      312 |  0.5.0| u V  |\n"
    "| MCUBOOT- | M55-HE | 0x8057D230 | 0x8057C830 | 0x58000000 | 0x58000000 |"
    "     4480 |  1.0.0| uLVB |\n"
)


class _FakeCfg:
    def __init__(self, bin_file, build_dir):
        self.bin_file = bin_file
        self.build_dir = build_dir


def _bin_with_reset_vector_bytes(rv: int, sp: int = 0x20080000) -> bytes:
    return sp.to_bytes(4, "little") + rv.to_bytes(4, "little") + b"\x00" * 8


def _make_runner(tmp_path, device="AE822FA0E5597LS0_HE", reset_vector=0x58000401,
                  maintenance_available=True, replace_atoc=False):
    setools = tmp_path / "setools"
    setools.mkdir()
    (setools / "app-gen-toc").write_bytes(b"")
    (setools / "app-write-mram").write_bytes(b"")
    if maintenance_available:
        (setools / "maintenance").write_bytes(b"")

    build_dir = tmp_path / "build"
    (build_dir / "zephyr").mkdir(parents=True)
    bin_file = build_dir / "zephyr" / "zephyr.bin"
    bin_file.write_bytes(_bin_with_reset_vector_bytes(reset_vector))

    cfg = _FakeCfg(bin_file=str(bin_file), build_dir=str(build_dir))
    runner = alif_flash.AlifFlashBinaryRunner(
        cfg, device, setools_dir=str(setools), se_uart="fake-uart",
        se_uart_baud="57600", replace_atoc=replace_atoc)
    # MEDIUM-4 review (#2262): set these on the INSTANCE, not just relying
    # on whatever `runners.core` stub happened to install them via
    # `__init__` above -- `test_rzv2n_mtd_flash_runner.py` installs its
    # OWN minimal `runners.core` stub (cfg only, no calls/logger/
    # check_call) guarded by the SAME `if "runners.core" not in
    # sys.modules` pattern this file uses, so whichever test module
    # imports first WINS the module-level stub for the whole pytest
    # session. Measured: `pytest tests/scripts/test_rzv2n_mtd_flash_runner.py
    # tests/scripts/test_alif_flash_runner.py` (that file first) gave 11
    # AttributeErrors here (`'AlifFlashBinaryRunner' object has no
    # attribute 'logger'/'calls'`) before this fix, since do_run's own
    # `self.logger.warning(...)` a few lines into every real run hit the
    # rzv2n stub's bare `self.cfg = cfg`. Setting these directly here
    # makes every runner this helper builds self-sufficient regardless of
    # import order.
    runner.calls = []
    runner.logger = logging.getLogger("test_alif_flash_runner")
    runner.check_call = lambda cmd, cwd=None: runner.calls.append((list(cmd), cwd))
    return runner


def _stub_maintenance(monkeypatch, banner=(_COMPLIANT_BANNER, 0), gettoc=("", 0)):
    def _fake(maintenance_path, se_uart, baud, opt):
        return banner if opt == "getbanner" else gettoc
    monkeypatch.setattr(alif_flash, "_run_maintenance", _fake)


def _write_mram_was_called(runner) -> bool:
    return any("app-write-mram" in cmd[0] for cmd, _cwd in runner.calls)


def _verdict(runner) -> dict:
    path = Path(runner.cfg.build_dir) / "alif_flash" / "atoc-guard.json"
    return json.loads(path.read_text(encoding="utf-8"))


def test_do_run_refuses_on_foreign_entry_and_never_burns(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_FOREIGN_GETTOC, 0))
    with pytest.raises(RuntimeError, match="REPLACES every app ATOC entry"):
        runner.do_run("flash")
    assert not _write_mram_was_called(runner)
    verdict = _verdict(runner)
    assert verdict["status"] == "refused-foreign"
    for entry in ("BOOTLOAD", "A32_APP", "HP_APP", "HE_APP"):
        assert entry in verdict["foreign"]


def test_do_run_refuses_on_factory_mcuboot_with_a_distinct_message(tmp_path, monkeypatch) -> None:
    # HIGH-2 review (#2262): a resident factory `MCUBOOT-` entry (a
    # pre-provisioned module) must NOT get the generic "re-run with
    # --replace-atoc" refusal text -- that steers an operator straight at
    # deleting the bootloader that makes the module boot at all. It must
    # instead name the entry, say what breaks, and point at the supported
    # Option B (plain J-Link, no ATOC) path.
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_FACTORY_MCUBOOT_GETTOC, 0))
    with pytest.raises(RuntimeError) as excinfo:
        runner.do_run("flash")
    message = str(excinfo.value)
    assert "MCUBOOT-" in message
    assert "unable to boot" in message
    assert "Option B" in message
    assert not _write_mram_was_called(runner)
    verdict = _verdict(runner)
    assert verdict["status"] == "refused-foreign"
    assert "MCUBOOT-" in verdict["foreign"]


def test_do_run_replace_atoc_still_overrides_the_factory_mcuboot_refusal(
        tmp_path, monkeypatch) -> None:
    # --replace-atoc remains the explicit override even for a factory
    # MCUBOOT- entry -- the guard warns, it does not hard-block.
    runner = _make_runner(tmp_path, replace_atoc=True)
    _stub_maintenance(monkeypatch, gettoc=(_FACTORY_MCUBOOT_GETTOC, 0))
    runner.do_run("flash")
    assert _write_mram_was_called(runner)
    verdict = _verdict(runner)
    assert verdict["status"] == "replaced"
    assert "MCUBOOT-" in verdict["foreign"]


def test_do_run_clean_board_proceeds_and_burns(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_CLEAN_HE_GETTOC, 0))
    runner.do_run("flash")
    assert _write_mram_was_called(runner)
    assert _verdict(runner)["status"] == "clear"


def test_do_run_no_atoc_found_proceeds_and_burns(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_NO_ATOC_TEXT, 0))
    runner.do_run("flash")
    assert _write_mram_was_called(runner)
    assert _verdict(runner)["status"] == "empty"


def test_do_run_refuses_when_maintenance_binary_missing(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path, maintenance_available=False)
    with pytest.raises(RuntimeError, match="Confirm by hand what is resident"):
        runner.do_run("flash")
    assert not _write_mram_was_called(runner)
    assert _verdict(runner)["status"] == "refused-unverified"


def test_do_run_unverified_refusal_warns_about_factory_mcuboot(tmp_path, monkeypatch) -> None:
    # Minor review fix (#2262): the unverified-read refusal must not
    # blindly steer an operator at --replace-atoc without first warning
    # that, on a pre-provisioned module, that flag can delist the factory
    # MCUBOOT- entry with no chance to see it named (the read never
    # succeeded, so refused-foreign's own naming never gets a chance to
    # run).
    runner = _make_runner(tmp_path, maintenance_available=False)
    with pytest.raises(RuntimeError) as excinfo:
        runner.do_run("flash")
    message = str(excinfo.value)
    assert "MCUBOOT-" in message
    assert "aen-provisioning.md" in message


def test_do_run_refuses_when_banner_is_missing_or_malformed(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, banner=("not a banner\n", 0), gettoc=(_CLEAN_HE_GETTOC, 0))
    with pytest.raises(RuntimeError, match="Confirm by hand what is resident"):
        runner.do_run("flash")
    assert not _write_mram_was_called(runner)


def test_do_run_refuses_when_gettoc_exits_nonzero(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_CLEAN_HE_GETTOC, 1))
    with pytest.raises(RuntimeError, match="Confirm by hand what is resident"):
        runner.do_run("flash")
    assert not _write_mram_was_called(runner)


def test_do_run_replace_atoc_overrides_foreign_entry_and_burns(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path, replace_atoc=True)
    _stub_maintenance(monkeypatch, gettoc=(_FOREIGN_GETTOC, 0))
    runner.do_run("flash")
    assert _write_mram_was_called(runner)
    assert _verdict(runner)["status"] == "replaced"


def test_do_run_replace_atoc_overrides_unverified_read_and_burns(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path, maintenance_available=False, replace_atoc=True)
    runner.do_run("flash")
    assert _write_mram_was_called(runner)
    assert _verdict(runner)["status"] == "replaced"


def test_do_run_allowed_entry_is_alp_he_for_an_he_build(tmp_path, monkeypatch) -> None:
    # A resident ALP-HP entry is foreign to an HE build's own write -- the
    # allowed set must be exactly {'ALP-HE'}, derived from the build's own
    # shape (_atoc_section_name), not a hardcoded/union set.
    runner = _make_runner(tmp_path, device="AE822FA0E5597LS0_HE",
                           reset_vector=0x58000401)
    hp_only = (
        "|   ALP-HP | M55-HP | 0x8057D230 | 0x8057C830 | 0x50000000 | 0x50000000 |"
        "     4480 |  1.0.0| uLVB |\n"
    )
    _stub_maintenance(monkeypatch, gettoc=(hp_only, 0))
    with pytest.raises(RuntimeError, match="REPLACES every app ATOC entry"):
        runner.do_run("flash")
    assert _verdict(runner)["foreign"] == ["ALP-HP"]
    assert _verdict(runner)["allowed"] == ["ALP-HE"]


def test_do_run_allowed_entry_is_alp_hp_for_an_hp_build(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path, device="AE822FA0E5597LS0_HP",
                           reset_vector=0x50000401)
    he_only = (
        "|   ALP-HE | M55-HE | 0x8057EDB0 | 0x8057E3B0 | 0x58000000 | 0x58000000 |"
        "     4480 |  1.0.0| uLVB |\n"
    )
    _stub_maintenance(monkeypatch, gettoc=(he_only, 0))
    with pytest.raises(RuntimeError, match="REPLACES every app ATOC entry"):
        runner.do_run("flash")
    assert _verdict(runner)["foreign"] == ["ALP-HE"]
    assert _verdict(runner)["allowed"] == ["ALP-HP"]


def test_do_run_writes_transcript_before_the_burn_step(tmp_path, monkeypatch) -> None:
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_CLEAN_HE_GETTOC, 0))
    runner.do_run("flash")
    transcript = Path(runner.cfg.build_dir) / "alif_flash" / "atoc-before.txt"
    assert transcript.is_file()
    assert "ALP-HE" in transcript.read_text(encoding="utf-8")


def test_do_run_removes_a_stale_verdict_file_when_it_fails_before_the_guard(
        tmp_path, monkeypatch) -> None:
    # MEDIUM-3 review (#2262): a run that fails BEFORE reaching the guard
    # (here: SETOOLS_DIR missing app-write-mram) must not leave a PREVIOUS
    # run's verdict.json behind for a caller to misread as this run's own
    # outcome.
    runner = _make_runner(tmp_path)
    _stub_maintenance(monkeypatch, gettoc=(_CLEAN_HE_GETTOC, 0))
    runner.do_run("flash")
    verdict_path = Path(runner.cfg.build_dir) / "alif_flash" / "atoc-guard.json"
    assert verdict_path.is_file()  # first run's own clean verdict

    # Second run: a failure AFTER the guard has already run once
    # successfully but BEFORE this attempt reaches the guard again is not
    # reachable here (the guard runs immediately before the burn, so
    # nothing sits between a repeat guard run and the previous one) --
    # simulate a failure that happens before do_run even gets to stage a
    # config instead: delete app-gen-toc, so the very first SETOOLS-dir
    # sanity check fails.
    (Path(runner.setools_dir) / "app-gen-toc").unlink()
    with pytest.raises(RuntimeError, match="does not look like a SETOOLS"):
        runner.do_run("flash")
    assert not verdict_path.exists(), (
        "a run that fails before the guard step must not leave a stale "
        "verdict.json from a PREVIOUS run behind"
    )
    # Deliberate contrast (nit review, #2262): the TRANSCRIPT is not
    # removed the same way -- its audit-trail value is being the last
    # successfully-read resident ATOC, whether or not this attempt got
    # far enough to read a new one.
    transcript_path = Path(runner.cfg.build_dir) / "alif_flash" / "atoc-before.txt"
    assert transcript_path.is_file(), (
        "atoc-before.txt from the previous successful read must survive "
        "a later run that fails before reaching the guard"
    )


# ---------------------------------------------------------------------
# _run_maintenance -- the SE-UART subprocess boundary (real executable,
# no monkeypatching of subprocess itself)
# ---------------------------------------------------------------------

_SKIP_ON_WINDOWS = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="a shebang script is not directly executable on Windows",
)


def _write_stub_script(path: Path, body: str) -> None:
    path.write_text(f"#!/usr/bin/env python3\n{body}", encoding="utf-8")
    path.chmod(0o755)


@_SKIP_ON_WINDOWS
def test_run_maintenance_passes_argv_and_cwd_and_merges_stderr(tmp_path) -> None:
    setools = tmp_path / "setools"
    setools.mkdir()
    maint = setools / "maintenance"
    _write_stub_script(maint, (
        "import os, sys\n"
        "print('cwd=' + os.getcwd())\n"
        "print('argv=' + repr(sys.argv[1:]))\n"
        "print('stderr line', file=sys.stderr)\n"
    ))
    text, rc = alif_flash._run_maintenance(maint, "fake-uart", "57600", "gettoc")
    assert rc == 0
    assert f"cwd={setools}" in text or f"cwd={setools.resolve()}" in text
    assert "argv=['-b', '57600', '-c', 'fake-uart', '-opt', 'gettoc']" in text
    assert "stderr line" in text  # stderr merged into the same transcript


@_SKIP_ON_WINDOWS
def test_run_maintenance_nonzero_exit_is_reported_verbatim(tmp_path) -> None:
    setools = tmp_path / "setools"
    setools.mkdir()
    maint = setools / "maintenance"
    _write_stub_script(maint, "import sys\nprint('partial')\nsys.exit(3)\n")
    text, rc = alif_flash._run_maintenance(maint, "fake-uart", "57600", "getbanner")
    assert rc == 3
    assert "partial" in text


def test_run_maintenance_missing_binary_returns_rc1_via_oserror(tmp_path) -> None:
    missing = tmp_path / "setools" / "maintenance"
    missing.parent.mkdir()
    text, rc = alif_flash._run_maintenance(missing, "fake-uart", "57600", "gettoc")
    assert rc == 1
    assert text  # some diagnostic text, not a bare empty string


@_SKIP_ON_WINDOWS
def test_run_maintenance_timeout_returns_rc1_with_partial_output(tmp_path, monkeypatch) -> None:
    # Fail-closed shape (MEDIUM-6 review, #2262): a wedged SE-UART must
    # not hang `west flash` forever. Monkeypatch the timeout constant down
    # so this test doesn't itself take 120s -- 1.5s (not e.g. 0.3s) to
    # leave headroom for `#!/usr/bin/env python3` interpreter startup on a
    # loaded CI host; measured flaky at 0.3s (the child's own flush never
    # lands before the kill).
    monkeypatch.setattr(alif_flash, "_MAINTENANCE_TIMEOUT_S", 1.5)
    setools = tmp_path / "setools"
    setools.mkdir()
    maint = setools / "maintenance"
    _write_stub_script(maint, (
        "import sys, time\n"
        "print('partial-before-hang')\n"
        "sys.stdout.flush()\n"
        "time.sleep(30)\n"
    ))
    text, rc = alif_flash._run_maintenance(maint, "fake-uart", "57600", "gettoc")
    assert rc == 1
    assert "partial-before-hang" in text
    assert "TIMEOUT" in text


# ---------------------------------------------------------------------
# do_create -- --se-uart-baud > $SE_UART_BAUD > 57600 precedence
# ---------------------------------------------------------------------


def _args_namespace(**overrides):
    base = dict(
        device=None, dev_id=None, setools_dir=None, se_uart=None,
        gen_toc="app-gen-toc", write_mram="app-write-mram",
        se_uart_baud=None, replace_atoc=False,
    )
    base.update(overrides)
    return types.SimpleNamespace(**base)


def test_do_create_se_uart_baud_defaults_to_57600(monkeypatch) -> None:
    monkeypatch.delenv("SE_UART_BAUD", raising=False)
    runner = alif_flash.AlifFlashBinaryRunner.do_create(object(), _args_namespace())
    assert runner.se_uart_baud == "57600"


def test_do_create_se_uart_baud_env_overrides_default(monkeypatch) -> None:
    monkeypatch.setenv("SE_UART_BAUD", "115200")
    runner = alif_flash.AlifFlashBinaryRunner.do_create(object(), _args_namespace())
    assert runner.se_uart_baud == "115200"


def test_do_create_se_uart_baud_flag_overrides_env(monkeypatch) -> None:
    monkeypatch.setenv("SE_UART_BAUD", "115200")
    runner = alif_flash.AlifFlashBinaryRunner.do_create(
        object(), _args_namespace(se_uart_baud="9600"))
    assert runner.se_uart_baud == "9600"
