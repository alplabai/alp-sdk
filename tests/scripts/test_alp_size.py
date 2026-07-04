# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the deterministic logic in
scripts/west_commands/alp_size.py.

All HW-free: no real build, no flash, no cross-toolchain.  The ELF
size extraction is exercised three ways -- a mocked `size` tool, the
host `size` tool on a host-compiled ELF (skipped when absent), and the
rom.json/ram.json footprint fallback -- plus the budget resolution
against the real SoM metadata in-repo, and the over-budget / unknown-
budget / not-built / n/a paths.

Run locally:

    python -m pytest tests/scripts/test_alp_size.py -q
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))
sys.path.insert(0, str(REPO / "scripts"))

import alp_size  # noqa: E402
from alp_size import (  # noqa: E402
    AlpSizeError,
    SliceSize,
    build_report,
    extract_sizes,
    find_size_tool,
    load_manifest,
    over_budget_rows,
    parse_berkeley_size,
    render_json,
    render_table,
    resolve_budget,
    slice_elf_path,
    unknown_budget_rows,
)


# ---------------------------------------------------------------------
# Fixtures / helpers
# ---------------------------------------------------------------------


def _write_manifest(build_root: Path, slices: list[dict],
                    hw_info: dict | None = None) -> Path:
    build_root.mkdir(parents=True, exist_ok=True)
    doc = {
        "schema_version": 1,
        "generated_by": "scripts/alp_orchestrate.py",
        "hw_info": hw_info if hw_info is not None else {"sku": "E1M-AEN801"},
        "slices": slices,
    }
    mpath = build_root / "system-manifest.yaml"
    mpath.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return mpath


class _FakeProc:
    def __init__(self, stdout: str, returncode: int = 0) -> None:
        self.stdout = stdout
        self.returncode = returncode


def _fake_size_run(flash_text: int, data: int, bss: int):
    """A `run` stand-in that emits Berkeley `size` output."""
    out = (f"   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
           f"   {flash_text}\t    {data}\t      {bss}\t   0\t    0\tx.elf\n")

    def run(_argv, **_kw):
        return _FakeProc(out)
    return run


def _touch_elf(build_root: Path, core: str = "m55_hp",
               os_: str = "zephyr") -> Path:
    """Create an empty placeholder zephyr.elf so .is_file() passes."""
    elf = build_root / f"{core}-{os_}" / "zephyr" / "zephyr.elf"
    elf.parent.mkdir(parents=True, exist_ok=True)
    elf.write_bytes(b"\x7fELF")
    return elf


# ---------------------------------------------------------------------
# Manifest loading
# ---------------------------------------------------------------------


def test_load_manifest_roundtrips(tmp_path):
    build_root = tmp_path / "build"
    _write_manifest(build_root, [{"core_id": "m55_hp", "os": "zephyr"}])
    data = load_manifest(build_root)
    assert data["hw_info"]["sku"] == "E1M-AEN801"


def test_load_manifest_missing_raises(tmp_path):
    with pytest.raises(AlpSizeError, match="no system-manifest.yaml"):
        load_manifest(tmp_path / "build")


def test_load_manifest_non_mapping_raises(tmp_path):
    build_root = tmp_path / "build"
    build_root.mkdir(parents=True)
    (build_root / "system-manifest.yaml").write_text("- a\n- b\n",
                                                     encoding="utf-8")
    with pytest.raises(AlpSizeError, match="top-level mapping"):
        load_manifest(build_root)


def test_slice_elf_path_uses_build_dir(tmp_path):
    build_root = tmp_path / "build"
    bd = build_root / "m55_hp-zephyr"
    s = {"core_id": "m55_hp", "os": "zephyr", "build_dir": str(bd)}
    assert slice_elf_path(s, build_root) == bd / "zephyr" / "zephyr.elf"


def test_slice_elf_path_reconstructs(tmp_path):
    build_root = tmp_path / "build"
    s = {"core_id": "m55_hp", "os": "zephyr"}
    assert slice_elf_path(s, build_root) == \
        build_root / "m55_hp-zephyr" / "zephyr" / "zephyr.elf"


# ---------------------------------------------------------------------
# Berkeley size parsing
# ---------------------------------------------------------------------


def test_parse_berkeley_size_basic():
    out = ("   text\t   data\t    bss\t    dec\t    hex\tfilename\n"
           "   1228\t    544\t      8\t   1780\t    6f4\tx.elf\n")
    # FLASH = text + data ; RAM = data + bss.
    assert parse_berkeley_size(out) == (1228 + 544, 544 + 8)


def test_parse_berkeley_size_no_data_row():
    assert parse_berkeley_size("text data bss dec hex filename\n") is None


def test_parse_berkeley_size_empty():
    assert parse_berkeley_size("") is None


# ---------------------------------------------------------------------
# extract_sizes -- source priority + fallbacks
# ---------------------------------------------------------------------


def test_extract_sizes_prefers_size_tool(tmp_path):
    elf = _touch_elf(tmp_path)
    build_dir = elf.parent.parent
    sizes, src = extract_sizes(elf, build_dir, "size",
                               run=_fake_size_run(1000, 200, 50))
    assert sizes == (1200, 250)
    assert src == "size-tool"


def test_extract_sizes_missing_elf_no_footprint(tmp_path):
    build_dir = tmp_path / "m55_hp-zephyr"
    elf = build_dir / "zephyr" / "zephyr.elf"
    sizes, src = extract_sizes(elf, build_dir, "size",
                               run=_fake_size_run(1, 1, 1))
    assert sizes is None and src is None


def test_extract_sizes_footprint_json_fallback(tmp_path):
    # No elf on disk -> fall through to rom.json + ram.json.
    build_dir = tmp_path / "m55_hp-zephyr"
    build_dir.mkdir(parents=True)
    (build_dir / "rom.json").write_text(
        json.dumps({"symbols": {"size": 4096}}), encoding="utf-8")
    (build_dir / "ram.json").write_text(
        json.dumps({"symbols": {"size": 2048}}), encoding="utf-8")
    elf = build_dir / "zephyr" / "zephyr.elf"
    sizes, src = extract_sizes(elf, build_dir, None)
    assert sizes == (4096, 2048)
    assert src == "rom/ram.json"


def test_extract_sizes_footprint_json_top_level_size(tmp_path):
    build_dir = tmp_path / "m55_hp-zephyr"
    build_dir.mkdir(parents=True)
    (build_dir / "rom.json").write_text(json.dumps({"size": 10}),
                                        encoding="utf-8")
    (build_dir / "ram.json").write_text(json.dumps({"size": 20}),
                                        encoding="utf-8")
    sizes, src = extract_sizes(build_dir / "zephyr" / "zephyr.elf",
                               build_dir, None)
    assert sizes == (10, 20)


def test_extract_sizes_partial_footprint_returns_none(tmp_path):
    build_dir = tmp_path / "m55_hp-zephyr"
    build_dir.mkdir(parents=True)
    (build_dir / "rom.json").write_text(json.dumps({"size": 10}),
                                        encoding="utf-8")
    # no ram.json
    sizes, src = extract_sizes(build_dir / "zephyr" / "zephyr.elf",
                               build_dir, None)
    assert sizes is None


@pytest.mark.skipif(shutil.which("cc") is None or shutil.which("size") is None,
                    reason="needs a host C compiler + binutils `size`")
def test_extract_sizes_real_host_size_tool(tmp_path):
    """End-to-end through the REAL `size` tool on a host-compiled ELF
    (no cross toolchain, no Zephyr build)."""
    src = tmp_path / "t.c"
    src.write_text("int main(void){return 0;}\n", encoding="utf-8")
    elf = tmp_path / "t.elf"
    subprocess.run(["cc", "-o", str(elf), str(src)], check=True)
    sizes, label = extract_sizes(elf, tmp_path, shutil.which("size"))
    assert sizes is not None
    flash, ram = sizes
    assert flash > 0 and ram >= 0
    assert label == "size-tool"


# ---------------------------------------------------------------------
# Budget resolution against the real in-repo metadata
# ---------------------------------------------------------------------


def test_resolve_budget_aen_m55_hp():
    flash, ram, note = resolve_budget("E1M-AEN801", "m55_hp", REPO)
    # E8 variant: 5.5 MB MRAM, m55_hp DTCM = 1024 KiB.
    assert flash == int(5.5 * 1024 * 1024)
    assert ram == 1024 * 1024
    assert note is None  # exact banks, no coarse fallback


def test_resolve_budget_aen_m55_he():
    flash, ram, _ = resolve_budget("E1M-AEN801", "m55_he", REPO)
    assert flash == int(5.5 * 1024 * 1024)
    assert ram == 256 * 1024


def test_resolve_budget_unknown_sku():
    flash, ram, note = resolve_budget("E1M-DOES-NOT-EXIST", "m55_hp", REPO)
    assert flash is None and ram is None
    assert "no SoM preset" in note


def test_resolve_budget_no_sku():
    flash, ram, note = resolve_budget(None, "m55_hp", REPO)
    assert flash is None and ram is None
    assert "no SKU" in note


# ---------------------------------------------------------------------
# build_report -- table assembly + status classification
# ---------------------------------------------------------------------


def _report(tmp_path, slices, sizes_by_core, sku="E1M-AEN801"):
    build_root = tmp_path / "build"
    _write_manifest(build_root, slices, hw_info={"sku": sku})
    for s in slices:
        if s.get("os") == "zephyr" and s["core_id"] in sizes_by_core:
            _touch_elf(build_root, s["core_id"])
    manifest = load_manifest(build_root)

    def fake_run(argv, **_kw):
        # argv = [size_bin, elf_path]; pick sizes by which core dir it's in.
        elf = Path(argv[1])
        core = elf.parent.parent.name.split("-")[0]
        flash, ram = sizes_by_core[core]
        # Berkeley: encode so text+data=flash, data+bss=ram (data=0).
        return _FakeProc(
            f"text data bss\n{flash} 0 {ram} 0\n")

    return build_report(manifest, build_root, REPO,
                        size_bin="size", run=fake_run)


def test_report_ok_under_budget(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1024 * 1024, 256 * 1024)},   # 5.5M / 1M budgets
    )
    assert len(rows) == 1
    r = rows[0]
    assert r.status == "ok"
    assert r.flash_used == 1024 * 1024
    assert r.flash_total == int(5.5 * 1024 * 1024)
    assert r.ram_total == 1024 * 1024


def test_report_over_budget_detected(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1024, 4 * 1024 * 1024)},   # RAM 4M > 1M budget
    )
    assert rows[0].status == "over"
    assert rows[0].over_budget
    assert over_budget_rows(rows) == [rows[0]]


def test_report_warn_near_budget(tmp_path):
    # RAM at 95% of the 1 MiB DTCM budget -> WARN (still fits).
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1024, int(0.95 * 1024 * 1024))},
    )
    assert rows[0].status == "warn"


def test_report_yocto_slice_is_na(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "a32_cluster", "os": "yocto"}],
        {},
    )
    assert rows[0].status == "n/a"
    assert rows[0].over_budget is False


def test_report_not_built_when_no_elf(tmp_path):
    # Zephyr slice declared but no elf / no footprint json on disk.
    build_root = tmp_path / "build"
    _write_manifest(build_root, [{"core_id": "m55_hp", "os": "zephyr"}])
    manifest = load_manifest(build_root)
    rows = build_report(manifest, build_root, REPO, size_bin="size",
                        run=_fake_size_run(1, 1, 1))
    assert rows[0].status == "not-built"
    # Budget still resolved + shown even though the image is absent.
    assert rows[0].flash_total == int(5.5 * 1024 * 1024)
    assert rows[0].flash_used is None


def test_report_unknown_budget_status(tmp_path):
    # Unknown SKU -> budget unresolvable, but the image IS measured.
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1000, 1000)},
        sku="E1M-NOT-REAL",
    )
    assert rows[0].status == "no-budget"
    assert rows[0].flash_total is None
    assert rows[0].flash_used == 1000
    assert unknown_budget_rows(rows) == [rows[0]]
    # A no-budget slice is NOT over budget (never fails the gate).
    assert not rows[0].over_budget


# ---------------------------------------------------------------------
# --fail-over-budget gate semantics
# ---------------------------------------------------------------------


def test_gate_skips_unknown_budget(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1000, 1000)},
        sku="E1M-NOT-REAL",
    )
    # Unknown budget must NOT count as over budget.
    assert over_budget_rows(rows) == []
    assert len(unknown_budget_rows(rows)) == 1


# ---------------------------------------------------------------------
# Rendering -- table + json
# ---------------------------------------------------------------------


def test_render_table_plain(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"},
         {"core_id": "a32_cluster", "os": "yocto"}],
        {"m55_hp": (1024 * 1024, 256 * 1024)},
    )
    table = render_table(rows, color=False)
    assert "CORE" in table and "FLASH used/total" in table and "STATUS" in table
    assert "m55_hp" in table
    assert "OK" in table
    assert "n/a" in table


def test_render_json_shape(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1024, 4 * 1024 * 1024)},   # over on RAM
    )
    doc = json.loads(render_json(rows))
    assert doc["schema"] == "alp-size/1"
    slc = doc["slices"][0]
    assert slc["core_id"] == "m55_hp"
    assert slc["status"] == "over"
    assert slc["flash"]["used"] == 1024
    assert slc["ram"]["total"] == 1024 * 1024
    assert slc["ram"]["pct"] is not None
    assert doc["summary"]["over_budget"] == ["m55_hp"]


def test_render_json_unknown_budget_listed(tmp_path):
    rows = _report(
        tmp_path,
        [{"core_id": "m55_hp", "os": "zephyr"}],
        {"m55_hp": (1000, 1000)},
        sku="E1M-NOT-REAL",
    )
    doc = json.loads(render_json(rows))
    assert doc["summary"]["unknown_budget"] == ["m55_hp"]
    assert doc["slices"][0]["flash"]["total"] is None


# ---------------------------------------------------------------------
# find_size_tool
# ---------------------------------------------------------------------


def test_find_size_tool_prefers_first_available():
    seen = []

    def which(name):
        seen.append(name)
        return "/usr/bin/llvm-size" if name == "llvm-size" else None

    assert find_size_tool(which=which) == "/usr/bin/llvm-size"
    # arm-zephyr-eabi-size is queried before llvm-size.
    assert seen[0] == "arm-zephyr-eabi-size"


def test_find_size_tool_none_when_absent():
    assert find_size_tool(which=lambda _n: None) is None
