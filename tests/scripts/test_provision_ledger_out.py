"""provision.ledger_out: unit.yaml merge (manual keys never overwritten, inline
'#' kept), md/log writing, ship check, manifest promotion, xlsx regen call."""

from __future__ import annotations

import sys
from datetime import datetime, timezone

import pytest
import yaml
from provision import ledger_out as lo

CAT = {
    "eeprom_unique_id": {"group": "identity", "source": "x", "mode": "auto", "ship_required": True},
    "uboot_version": {"group": "firmware", "source": "x", "mode": "auto", "ship_required": True},
    "dram_part": {"group": "memory", "source": "op", "mode": "manual", "ship_required": False},
    "disposition": {"group": "disposition", "source": "op", "mode": "manual", "ship_required": True},
    "test_*": {"group": "campaign", "source": "hil", "mode": "auto", "ship_required": False},
}
UNIT = """# header comment
dram_part: vendor part (see repo#1234)
uboot_version: old
  # indented comment
notes: keep # this
"""


def test_merge_keeps_manual_and_order(tmp_path):
    p = tmp_path / "u.unit.yaml"
    p.write_text(UNIT, encoding="utf-8")
    changed = lo.merge_unit_yaml(p, {"uboot_version": "U-Boot 2024.07", "dram_part": "EVIL",
                                     "eeprom_unique_id": "06 03", "test_i2c": "pass"}, CAT)
    assert changed == ["uboot_version", "eeprom_unique_id", "test_i2c"]
    text = p.read_text(encoding="utf-8")
    assert text.splitlines()[:5] == ["# header comment", "dram_part: vendor part (see repo#1234)",
                                     "uboot_version: U-Boot 2024.07", "  # indented comment",
                                     "notes: keep # this"]
    assert text.endswith("eeprom_unique_id: 06 03\ntest_i2c: pass\n") and "\r" not in text
    assert lo.read_unit_yaml(p)["notes"] == "keep # this"
    assert lo.merge_unit_yaml(p, {"uboot_version": "U-Boot 2024.07"}, CAT) == []


def test_merge_defaults_only_fill_absent_keys(tmp_path):
    p = tmp_path / "u.unit.yaml"
    assert lo.merge_unit_yaml(p, {}, CAT, {"disposition": "bench-only"}) == ["disposition"]
    p.write_text("disposition: ship\n", encoding="utf-8")
    assert lo.merge_unit_yaml(p, {"disposition": "scrap"}, CAT, {"disposition": "bench-only"}) == []
    assert lo.read_unit_yaml(p)["disposition"] == "ship"


def test_merge_refuses_newline(tmp_path):
    with pytest.raises(ValueError):
        lo.merge_unit_yaml(tmp_path / "u.yaml", {"uboot_version": "a\nb"}, CAT)


def test_ship_check():
    ok = {"eeprom_unique_id": "06", "uboot_version": "U", "disposition": "ship", "known_defects": "none"}
    assert lo.ship_check(ok, CAT) == []
    r = lo.ship_check({**ok, "act88760_gpio4_defect": "yes", "disposition": "bench-only",
                       "uboot_version": ""}, CAT)
    assert "missing uboot_version" in r
    assert any("gpio4_defect" in x for x in r) and any("bench-only" in x for x in r)
    assert lo.ship_check({**ok, "known_defects": "cracked"}, CAT) == ["known_defects: cracked"]
    assert lo.ship_check({**ok, "provision_overrides": "tier_triangle: rework"}, CAT) ==         ["provision_overrides: tier_triangle: rework"]
    assert lo.ship_check({**ok, "rootfs_bundle_version": "build-dir:deploy"}, CAT)


def test_catalogue_loader(tmp_path):
    p = tmp_path / "v2n.keys.yaml"
    p.write_text(yaml.safe_dump({"schema": 1, "family": "v2n", "keys": CAT}), encoding="utf-8")
    assert lo.load_catalogue(p)["dram_part"]["mode"] == "manual"
    p.write_text(yaml.safe_dump({"schema": 1, "keys": {"k": {"mode": "auto"}}}), encoding="utf-8")
    with pytest.raises(ValueError):
        lo.load_catalogue(p)


def test_md_log_promote(tmp_path):
    md = tmp_path / "E1M-X" / "2026W38-0001.md"
    lo.append_md_section(md, "run", "- a\n", datetime(2026, 9, 24, 12, tzinfo=timezone.utc))
    assert "## run (2026-09-24 12:00:00Z)" in md.read_text(encoding="utf-8")
    log = lo.write_log(tmp_path, "E1M-X", "2026W38-0001", "census", "hello")
    assert log.parent == tmp_path / "E1M-X" / "logs" / "2026W38-0001" and log.name.startswith("census-")
    d = tmp_path / "E1M-X"
    (d / "2026W38-0001.manifest.staged.bin").write_bytes(b"m")
    assert lo.promote_manifest(d, "2026W38-0001").read_bytes() == b"m"
    (d / "2026W38-0001.manifest.staged.bin").write_bytes(b"n")
    with pytest.raises(ValueError, match="already exists"):
        lo.promote_manifest(d, "2026W38-0001")


def test_regen_xlsx_invokes_tool(tmp_path):
    tool = tmp_path / "ledger_xlsx.py"
    tool.write_text("import sys; print(' '.join(sys.argv[1:]))\n", encoding="utf-8")
    p = lo.regen_xlsx(tmp_path / "ledger", tool, tmp_path / "out.xlsx")
    assert p.returncode == 0 and "--ledger-root" in p.stdout and "out.xlsx" in p.stdout
    assert sys.executable
