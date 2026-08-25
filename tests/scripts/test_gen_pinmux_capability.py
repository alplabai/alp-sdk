"""Unit tests for scripts/gen_pinmux_capability.py.

Covers the committed-file-in-sync invariant, the v2n `core` field projected
from metadata/e1m_modules/v2n/core-ownership.yaml (issue #1157), and the
three hard-error paths that keep that file honest: a core-ownership entry
that matches zero rows, two entries duplicating the same (peripheral, pad)
key in the ownership file, and one entry whose key matches two emitted rows.
"""

import subprocess
import sys
from pathlib import Path

import gen_pinmux_capability as gpc  # noqa: E402  (scripts/ on sys.path via conftest)
import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_pinmux_capability.py"


def test_committed_files_match_generator():
    for family, spec in gpc.FAMILIES.items():
        pads = gpc._pads_for_family(spec)
        text = gpc._render(family, spec, pads)
        out = gpc.PINMUX_DIR / f"{family}.yaml"
        assert out.read_text(encoding="utf-8") == text, (
            f"{out} is stale -- run `python3 scripts/gen_pinmux_capability.py`")


def test_check_mode_passes_on_committed_files():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"], capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_v2n_core_field_matches_verified_ownership_only():
    """The `core` field emitted on each pad matches core-ownership.yaml
    exactly -- the contract is "the ownership file", not a hardcoded pad
    set, so this test derives its expectation from that file rather than
    pinning today's pad list (issue #1157, 2026-08-12 comment: a hardcoded
    `m33_pads == {...}` would go red the moment a newly-verified pad is
    added and force the next person to argue with the test instead of the
    data). Also asserts the two invariants the issue does want fixed: no
    `a55` row yet, and every `core`-tagged row is `owner: "renesas"`."""
    doc = yaml.safe_load((gpc.PINMUX_DIR / "v2n.yaml").read_text(encoding="utf-8"))
    ownership = yaml.safe_load(
        (REPO / "metadata" / "e1m_modules" / "v2n" / "core-ownership.yaml")
        .read_text(encoding="utf-8"))
    expected = {
        (entry["peripheral"], entry["pad"]): entry["core"]
        for entry in ownership["core_ownership"]
    }
    actual = {
        (p["silicon_peripheral"], p["silicon_pad"]): p["core"]
        for p in doc["pads"]
        if "core" in p
    }
    assert actual == expected
    # No row anywhere claims "a55" -- nothing in this batch is verified a55.
    assert not any(p.get("core") == "a55" for p in doc["pads"])
    # Every m33 row is renesas-owned (the AMP-core ambiguity is a renesas
    # fact; the GD32's own pads never carry `core`).
    for p in doc["pads"]:
        if "core" in p:
            assert p["owner"] == "renesas"


def _write_core_ownership(tmp_path: Path, entries: list[dict]) -> Path:
    modules = tmp_path / "e1m_modules"
    v2n_dir = modules / "v2n"
    v2n_dir.mkdir(parents=True)
    # Copy the real renesas/gd32 TSVs so row lookups still resolve.
    real_v2n = REPO / "metadata" / "e1m_modules" / "v2n"
    for name in ("renesas-peripheral-map.tsv", "gd32-io-mcu-map.tsv"):
        (v2n_dir / name).write_text(
            (real_v2n / name).read_text(encoding="utf-8"), encoding="utf-8")
    ownership = v2n_dir / "core-ownership.yaml"
    ownership.write_text(
        yaml.safe_dump({"core_ownership": entries}), encoding="utf-8")
    return modules


def test_unmatched_core_ownership_entry_is_a_hard_error(tmp_path, monkeypatch):
    modules = _write_core_ownership(tmp_path, [
        {"peripheral": "RIIC8_SDA8", "pad": "P06X", "core": "m33"},
    ])
    monkeypatch.setattr(gpc, "MODULES", modules)
    spec = gpc.FAMILIES["v2n"]
    with pytest.raises(SystemExit, match="matched no row"):
        gpc._pads_for_family(spec)


def test_duplicate_core_ownership_key_is_a_hard_error(tmp_path, monkeypatch):
    modules = _write_core_ownership(tmp_path, [
        {"peripheral": "RIIC8_SDA8", "pad": "P06", "core": "m33"},
        {"peripheral": "RIIC8_SDA8", "pad": "P06", "core": "a55"},
    ])
    monkeypatch.setattr(gpc, "MODULES", modules)
    spec = gpc.FAMILIES["v2n"]
    with pytest.raises(SystemExit, match="more than once"):
        gpc._pads_for_family(spec)


def test_core_ownership_entry_matching_two_rows_is_a_hard_error(tmp_path, monkeypatch):
    """A single, non-duplicated ownership entry whose (peripheral, pad) key
    is emitted by TWO source rows is ambiguous and must also hard-error --
    distinct from the duplicate-key-in-the-ownership-file case above."""
    modules = tmp_path / "e1m_modules"
    v2n_dir = modules / "v2n"
    v2n_dir.mkdir(parents=True)
    (v2n_dir / "renesas-peripheral-map.tsv").write_text(
        "peripheral\trenesas_pad\n"
        "RIIC8_SDA8\tP06\n"
        "RIIC8_SDA8\tP06\n",
        encoding="utf-8")
    (v2n_dir / "gd32-io-mcu-map.tsv").write_text(
        "peripheral\tgd32_pad\n", encoding="utf-8")
    (v2n_dir / "core-ownership.yaml").write_text(
        yaml.safe_dump({"core_ownership": [
            {"peripheral": "RIIC8_SDA8", "pad": "P06", "core": "m33"},
        ]}), encoding="utf-8")
    monkeypatch.setattr(gpc, "MODULES", modules)
    spec = gpc.FAMILIES["v2n"]
    with pytest.raises(SystemExit, match="matches more than one row"):
        gpc._pads_for_family(spec)
