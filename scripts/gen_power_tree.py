#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate include/alp/chips/<family>_power_tree.h -- the runtime guard tables
for the on-module PMIC / regulator drivers (ACT88760, DA9292, TPS628640) --
from metadata/e1m_modules/<family>/power-tree.yaml.

The drivers are fail-closed: with no limits table installed every control
write returns ALP_ERR_NOSUPPORT.  The table they need (per-rail window,
critical flag, voltage/enable writability, GPIO polarity mask) is a HW fact
that must live once, in metadata -- so it is projected here as brace
initialisers (macros, no storage) that firmware instantiates in its own
static storage and hands to act8760_set_limits() / da9292_set_limits() /
tps628640_set_limits().  One set of tables per family listed in the power
tree (`v2n` -> V2N_POWER_*, `v2n-m1` -> V2N_M1_POWER_*); a rail absent from
a family gets the all-zero "no control" entry.

This module also owns the cross-file semantic check the JSON schema cannot
express (cross_check()): scripts/validate_metadata.py calls it, so the
window arithmetic used to validate is the same code that generates.

Run:

    python3 scripts/gen_power_tree.py          # regenerate
    python3 scripts/gen_power_tree.py --check  # exit 1 if a header is stale

CI (pr-generated-files.yml) regenerates and fails on any diff.
"""

from __future__ import annotations

import argparse
import math
import shutil
import subprocess
import sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
# SoM preset `family:` (vendor-style) -> short family slug; one shared table.
from check_example_portability import _VENDOR_FAMILY_TO_SLUG  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
E1M_MODULES = REPO / "metadata" / "e1m_modules"
CHIPS = REPO / "metadata" / "chips"
OUT_DIR = REPO / "include" / "alp" / "chips"

CHIP_IDS = ("act8760", "da9292", "tps628640")

# control class -> (voltage_writable, enable_writable).  "voltage" grants
# ONLY a voltage write -- it does NOT imply enable_writable, because most
# "voltage" rails (every ACT88760 CMI-sequenced rail in the V2N power tree)
# are switched by the PMIC's own hardware sequence and were never meant to
# be software-enabled/disabled over I2C.  A rail that genuinely needs both
# (e.g. a TPS628640 buck whose SOFTWARE_ENABLE bit is a documented guarded
# API) opts in explicitly with "voltage_enable" -- reviewed per rail, not a
# default.
_CONTROL_FLAGS = {
    "voltage": (True, False),
    "voltage_enable": (True, True),
    "sequence": (True, True),
    "enable": (False, True),
    "none": (False, False),
}


def load_tree(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def load_chips(chips_dir: Path = CHIPS) -> dict:
    return {c: yaml.safe_load((chips_dir / f"{c}.yaml").read_text(encoding="utf-8")) for c in CHIP_IDS}


def tree_paths() -> list[Path]:
    return sorted(E1M_MODULES.glob("*/power-tree.yaml"))


def chip_channel(chips: dict, rail: dict) -> dict | None:
    """The chip-manifest rail/channel entry a power-tree rail resolves to."""
    doc = chips.get(rail["chip"]) or {}
    entries = doc.get("rails") if rail["chip"] == "act8760" else doc.get("channels")
    for e in entries or []:
        if e.get("id") == rail["channel"]:
            return e
    return None


def grid(chips: dict, rail: dict) -> tuple[int, int]:
    """(base_uv, step_uv) of the VSET grid the rail is programmed on."""
    ch = chip_channel(chips, rail)
    if rail["chip"] == "act8760":
        for r in ch["ranges"]:
            if r["range"] == rail["range"]:
                return r["base_mv"] * 1000, r["step_uv"]
        raise KeyError(f"{rail['id']}: act8760 {rail['channel']} has no range {rail['range']}")
    if rail["chip"] == "da9292":
        return ch["abs_min_mv"] * 1000, ch["step_mv"] * 1000
    return ch["base_mv"] * 1000, ch["step_mv"] * 1000


def default_window(target_mv: int, base_uv: int, step_uv: int, pct: float) -> list[int]:
    """target +/-pct %, rounded INWARD to the VSET grid, then to whole mV
    (min up, max down) -- the guard can only ever be tighter than +/-pct."""
    lo_uv = target_mv * 1000 * (100 - pct) / 100
    hi_uv = target_mv * 1000 * (100 + pct) / 100
    lo_code = math.ceil((lo_uv - base_uv) / step_uv)
    hi_code = math.floor((hi_uv - base_uv) / step_uv)
    lo = base_uv + lo_code * step_uv
    hi = base_uv + hi_code * step_uv
    return [math.ceil(lo / 1000), math.floor(hi / 1000)]


def limit_entry(rail: dict | None) -> dict:
    if rail is None:
        return {"min_mv": 0, "max_mv": 0, "critical": False, "voltage_writable": False, "enable_writable": False}
    v_ok, en_ok = _CONTROL_FLAGS[rail["control"]]
    win = rail["window_mv"] or [0, 0]
    return {
        "min_mv": win[0],
        "max_mv": win[1],
        "critical": rail["critical"],
        "voltage_writable": v_ok and rail["window_mv"] is not None,
        "enable_writable": en_ok,
    }


def _deny_addrs(table: list, slave: str | None) -> set[int]:
    out: set[int] = set()
    for row in table or []:
        if row.get("write") != "deny" or row.get("slave", slave) != slave:
            continue
        if "addr_range" in row:
            lo, hi = row["addr_range"]
            out.update(range(lo, hi + 1))
        else:
            out.add(row["addr"])
    return out


def load_ownership(tree_path: Path) -> dict | None:
    """The family's core-ownership.yaml next to its power tree, if any."""
    p = tree_path.parent / "core-ownership.yaml"
    return yaml.safe_load(p.read_text(encoding="utf-8")) if p.is_file() else None


def _core_for(rows: dict, peripheral: str, mode: str) -> str | None:
    """The core that drives `peripheral` DURING boot mode `mode`: its
    core-ownership.yaml `boot_mode_core[mode]` if that qualifier is
    present, else its flat `core` (unqualified rows drive the same core in
    every boot mode -- unchanged behaviour for every row that never adds
    the qualifier)."""
    row = rows.get(peripheral)
    if row is None:
        return None
    per_mode = row.get("boot_mode_core") or {}
    return per_mode.get(mode, row.get("core"))


def _check_boot_modes(tree: dict, ownership: dict | None) -> list[str]:
    """Boot-mode owners vs the AMP core-ownership policy: a boot mode may
    only name cm33 as BRD_I2C master / DEEPX sequence owner when
    core-ownership.yaml gives the RIIC8 pads (and, for the sequence, P64 /
    P65) to the m33 FOR THAT BOOT MODE (`boot_mode_core`, falling back to
    the flat `core`) -- otherwise two masters could share one RIIC8 block.
    A pad whose ownership is genuinely time-sliced (the CM33 masters it
    only until it releases the CA55, then the A55 takes over) is not
    "concurrent": `boot_mode_core` names one core per mode, never both."""
    errs: list[str] = []
    rows = {r["peripheral"]: r for r in (ownership or {}).get("core_ownership") or []}
    for mode, bm in tree["boot_modes"].items():
        rail_owners = {r["owner"][mode] for r in tree["rails"]}
        if bm.get("status") == "blocked":
            if bm["deepx_sequence_owner"] != "none" or "cm33" in rail_owners:
                errs.append(f"boot_modes.{mode}: blocked, so deepx_sequence_owner and every rail owner "
                            f"must not name a runtime owner (got {bm['deepx_sequence_owner']} / {sorted(rail_owners)})")
            continue
        if ownership is None:
            continue
        if bm["bus_master"] == "cm33" and any(
                _core_for(rows, k, mode) != "m33" for k in ("RIIC8_SCL8", "RIIC8_SDA8")):
            errs.append(f"boot_modes.{mode}: bus_master cm33 but core-ownership.yaml gives RIIC8 to "
                        f"{_core_for(rows, 'RIIC8_SCL8', mode)} for this boot mode "
                        f"(the CM33 must not master RIIC8)")
        if (bm["deepx_sequence_owner"] == "cm33" or "cm33" in rail_owners) and any(
                _core_for(rows, k, mode) != "m33" for k in ("DEEPX_CORE_0P75_EN", "DEEPX_PWR_EN_REQ")):
            errs.append(f"boot_modes.{mode}: cm33 owns the DEEPX sequence but core-ownership.yaml gives "
                        f"P64/P65 to {_core_for(rows, 'DEEPX_CORE_0P75_EN', mode)} for this boot mode")
    return errs


def cross_check(tree: dict, chips: dict, som_presets: dict[str, dict],
                ownership: dict | None = None) -> list[str]:
    """Semantic checks beyond the schema.  som_presets: {sku: preset doc};
    ownership: the family's core-ownership.yaml (see load_ownership())."""
    errs: list[str] = _check_boot_modes(tree, ownership)
    pct = tree["window_policy"]["default_tolerance_pct"]
    families = set(tree["families"])
    seen_ids: set[str] = set()
    for rail in tree["rails"]:
        rid = rail["id"]
        if rid in seen_ids:
            errs.append(f"rail {rid}: duplicate id")
        seen_ids.add(rid)
        ch = chip_channel(chips, rail)
        if ch is None:
            errs.append(f"rail {rid}: {rail['chip']} has no rail/channel '{rail['channel']}'")
            continue
        if not set(rail["populated_on"]) <= families:
            errs.append(f"rail {rid}: populated_on names a family not in `families`")
        # address vs chip manifest
        c = rail["chip"]
        if c == "act8760":
            want = tree["chips"]["act8760"]["addr_add1" if ch["slave"] == "add1" else "addr_add2"]
            if rail["i2c_addr"] != want:
                errs.append(f"rail {rid}: i2c_addr 0x{rail['i2c_addr']:02X} != act8760 {ch['slave']} 0x{want:02X}")
        elif c == "da9292":
            if rail["i2c_addr"] != tree["chips"]["da9292"]["addr"]:
                errs.append(f"rail {rid}: i2c_addr != chips.da9292.addr")
        else:
            rows = {a["addr_7bit"]: a for a in chips["tps628640"]["i2c"]["addresses"]}
            row = rows.get(rail["i2c_addr"])
            if row is None:
                errs.append(f"rail {rid}: 0x{rail['i2c_addr']:02X} not in tps628640.yaml i2c.addresses")
            elif row.get("voltage_mv") != rail["target_mv"] or row.get("rail") != rail["net"]:
                errs.append(f"rail {rid}: tps628640.yaml 0x{rail['i2c_addr']:02X} says "
                            f"{row.get('rail')}/{row.get('voltage_mv')} mV, power tree says "
                            f"{rail['net']}/{rail['target_mv']} mV")
        # address present on BRD_I2C of every SoM preset of each populated family
        for sku, preset in som_presets.items():
            if _VENDOR_FAMILY_TO_SLUG.get(preset.get("family")) not in rail["populated_on"]:
                continue
            devs = (((preset.get("on_module") or {}).get("i2c_devices") or {}).get(tree["bus"]) or {}).get("devices") or []
            if not any(d.get("chip") == c and int(str(d.get("address_7bit")), 0) == rail["i2c_addr"] for d in devs):
                errs.append(f"rail {rid}: {sku} declares no {c} at 0x{rail['i2c_addr']:02X} on {tree['bus']}")
        # window policy
        writable = rail["control"] in ("voltage", "voltage_enable", "sequence")
        if rail["target_mv"] is None or rail["window_mv"] is None:
            if writable:
                errs.append(f"rail {rid}: control '{rail['control']}' needs target_mv and window_mv (TBD => control: none)")
            if (rail["target_mv"] is None) != (rail["window_mv"] is None) and rail["control"] != "enable":
                errs.append(f"rail {rid}: target_mv and window_mv must both be set or both null")
            continue
        if c == "act8760" and rail["range"] is None:
            errs.append(f"rail {rid}: act8760 rail needs `range`")
            continue
        base_uv, step_uv = grid(chips, rail)
        want = default_window(rail["target_mv"], base_uv, step_uv, pct)
        if rail["window_mv"] != want:
            errs.append(f"rail {rid}: window_mv {rail['window_mv']} != default {want} "
                        f"(target {rail['target_mv']} mV +/-{pct} % inward on the {step_uv} uV grid)")
        lo, hi = rail["window_mv"]
        if not (lo <= rail["target_mv"] <= hi):
            errs.append(f"rail {rid}: target outside its own window")
        if lo < ch["abs_min_mv"] or hi > ch["abs_max_mv"]:
            errs.append(f"rail {rid}: window {rail['window_mv']} outside chip limits "
                        f"[{ch['abs_min_mv']}, {ch['abs_max_mv']}]")
    # never_write (write: deny rows) disjoint from rail-owned registers
    act = chips["act8760"]
    for slave in ("add1", "add2"):
        deny = _deny_addrs(act.get("register_table"), slave)
        for r in act["rails"]:
            if r["slave"] != slave:
                continue
            owned = {r["status_reg"], r["vset0_reg"], r["enable_reg"], *r["dvs_regs"]}
            if r["range_reg"] is not None:
                owned.add(r["range_reg"])
            if owned & deny:
                errs.append(f"act8760 {r['id']}: registers {sorted(owned & deny)} are both rail-owned and write: deny")
    for c in ("da9292", "tps628640"):
        deny = _deny_addrs(chips[c].get("register_table"), None)
        for ch in chips[c].get("channels") or []:
            owned = {ch.get(k) for k in ("ctrl_reg", "vsel_lo_reg", "vsel_hi_reg", "vout_reg")} - {None}
            if owned & deny:
                errs.append(f"{c} {ch['id']}: registers {sorted(owned & deny)} are both channel-owned and write: deny")
    # GPIOs
    gpios = [g["gpio"] for g in tree["gpios"]]
    if sorted(gpios) != list(range(1, 12)):
        errs.append(f"gpios: must list GPIO1..GPIO11 exactly once, got {sorted(gpios)}")
    return errs


# --------------------------------------------------------------------------
# C emission


def _prefix(family: str) -> str:
    return family.upper().replace("-", "_") + "_POWER"


def _c_ident(net: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in net).upper()


def _entry_c(e: dict) -> str:
    b = lambda v: "true" if v else "false"  # noqa: E731
    return (f"{{ .min_mv = {e['min_mv']}u, .max_mv = {e['max_mv']}u, .critical = {b(e['critical'])}, "
            f".voltage_writable = {b(e['voltage_writable'])}, .enable_writable = {b(e['enable_writable'])} }}")


def _macro(name: str, rows: list[str]) -> list[str]:
    out = [f"#define {name} \\", "\t{ \\"]
    out += [f"\t\t{r}, \\" for r in rows]
    out += ["\t}", ""]
    return out


def render(tree: dict, family_dir: str) -> str:
    rails = tree["rails"]
    act_ids = [f"buck{i}" for i in range(1, 8)] + [f"ldo{i}" for i in range(1, 7)]
    guard = f"ALP_CHIPS_{family_dir.upper().replace('-', '_')}_POWER_TREE_H"
    src = f"metadata/e1m_modules/{family_dir}/power-tree.yaml"
    L = [
        "/*",
        " * Copyright 2026 Alp Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        f" * Auto-generated by scripts/gen_power_tree.py from {src}.",
        " * DO NOT EDIT BY HAND -- regenerate:",
        " *   python3 scripts/gen_power_tree.py",
        " */",
        "",
        "/**",
        f" * @file {family_dir}_power_tree.h",
        f" * @brief Generated runtime guard tables for the {family_dir.upper()}-family on-module",
        " *        PMIC / regulator drivers (ACT88760, DA9292, TPS628640).",
        " *",
        " * Brace initialisers only (no storage): instantiate in static storage and",
        " * install with act8760_set_limits() / da9292_set_limits() /",
        " * tps628640_set_limits().  See <alp/chips/pmic_rail_limit.h> for the guard",
        " * semantics and the source YAML for nets, targets and provenance.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "alp/chips/pmic_rail_limit.h"',
        '#include "alp/chips/act8760.h"',
        '#include "alp/chips/da9292.h"',
        '#include "alp/chips/tps628640.h"',
        "",
    ]
    ident = tree["chips"]["da9292"]["identity"]
    fam0 = _prefix(tree["families"][0])
    L += [
        "/** Expected DA9292 PMC_DEV_ID (0x19). */",
        f"#define {fam0}_DA9292_DEV_ID  0x{ident['dev_id']:02X}u",
        "/** Expected DA9292 PMC_REV_ID (0x1A). */",
        f"#define {fam0}_DA9292_REV_ID  0x{ident['rev_id']:02X}u",
        "/** Expected DA9292 PMC_CFG_REV (0x1B). */",
        f"#define {fam0}_DA9292_CFG_REV 0x{ident['cfg_rev']:02X}u",
        "",
    ]
    # ACT GPIO facts (family-independent: one PCB)
    mask = 0
    for g in sorted(tree["gpios"], key=lambda g: g["gpio"]):
        if "polarity" in g["writable"]:
            mask |= 1 << (g["gpio"] - 1)
    L += ["/** ACT88760 GPIOs whose MODEx polarity bit may be written: bit (n-1) = GPIOn. */",
          f"#define {fam0}_ACT8760_GPIO_POLARITY_WRITABLE_MASK 0x{mask:04X}u", ""]
    for g in sorted(tree["gpios"], key=lambda g: g["gpio"]):
        if "expected_mode" in g:
            L += [f"/** Expected ACT88760 MODE{g['gpio']} byte ({g['net']}). */",
                  f"#define {fam0}_ACT8760_GPIO{g['gpio']}_EXPECTED_MODE 0x{g['expected_mode']:02X}u", ""]
    L += [f"/** ACT88760 GPIO net names, index = GPIO number - 1. */"]
    L += _macro(f"{fam0}_ACT8760_GPIO_NETS_INIT",
                [f'"{g["net"]}"' for g in sorted(tree["gpios"], key=lambda g: g["gpio"])])

    for fam in tree["families"]:
        pre = _prefix(fam)
        pop = [r for r in rails if fam in r["populated_on"]]
        by = {(r["chip"], r["channel"]): r for r in pop}
        # ACT
        rows = [f"[ACT8760_RAIL_{cid.upper()}] = {_entry_c(limit_entry(by.get(('act8760', cid))))}" for cid in act_ids]
        L += [f"/** {fam}: ACT88760 limits, indexed by act8760_rail_t. */"]
        L += _macro(f"{pre}_ACT8760_RAIL_LIMITS_INIT", rows)
        nets = [f'[ACT8760_RAIL_{cid.upper()}] = "{(by.get(("act8760", cid)) or {}).get("net", "")}"' for cid in act_ids]
        L += [f"/** {fam}: ACT88760 rail net names, indexed by act8760_rail_t. */"]
        L += _macro(f"{pre}_ACT8760_RAIL_NETS_INIT", nets)
        # DA9292
        rows = [f"[DA9292_{ch.upper()}] = {_entry_c(limit_entry(by.get(('da9292', ch))))}" for ch in ("ch1", "ch2")]
        L += [f"/** {fam}: DA9292 limits, indexed by da9292_channel_t. */"]
        L += _macro(f"{pre}_DA9292_CH_LIMITS_INIT", rows)
        nets = [f'[DA9292_{ch.upper()}] = "{(by.get(("da9292", ch)) or {}).get("net", "")}"' for ch in ("ch1", "ch2")]
        L += [f"/** {fam}: DA9292 channel net names, indexed by da9292_channel_t. */"]
        L += _macro(f"{pre}_DA9292_CH_NETS_INIT", nets)
        # TPS628640 instances
        for r in sorted((r for r in pop if r["chip"] == "tps628640"), key=lambda r: r["i2c_addr"]):
            n = _c_ident(r["net"])
            opt = " (assembled: optional)" if r.get("assembled") == "optional" else ""
            L += [f"/** {fam}: TPS628640 `{r['net']}` 7-bit address{opt}. */",
                  f"#define {pre}_TPS628640_{n}_ADDR 0x{r['i2c_addr']:02X}u",
                  f"/** {fam}: TPS628640 `{r['net']}` limit entry. */",
                  f"#define {pre}_TPS628640_{n}_LIMIT_INIT {_entry_c(limit_entry(r))}",
                  f"/** {fam}: TPS628640 `{r['net']}` net name. */",
                  f'#define {pre}_TPS628640_{n}_NET "{r["net"]}"', ""]
    L += [f"#endif /* {guard} */", ""]
    return "\n".join(L)


def _clang_format_exe() -> str:
    """Pinned clang-format, or exit 99 (test-all.sh maps rc 99 to SKIP)."""
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if exe is None:
        print("error: clang-format not found on PATH (install clang-format==22.* -- see docs/testing.md)",
              file=sys.stderr)
        raise SystemExit(99)
    return exe


def out_path_for(family_dir: str) -> Path:
    """Single indirection point so a test can redirect output to a tmp dir."""
    return OUT_DIR / f"{family_dir}_power_tree.h"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if a generated header is stale; write nothing")
    args = ap.parse_args(argv)
    paths = tree_paths()
    if not paths:
        print("gen_power_tree: no metadata/e1m_modules/*/power-tree.yaml", file=sys.stderr)
        return 1
    exe = _clang_format_exe()
    stale = 0
    for p in paths:
        fam_dir = p.parent.name
        out = out_path_for(fam_dir)
        text = subprocess.run([exe, f"--style=file:{REPO / '.clang-format'}", "--assume-filename=x.h"],
                              input=render(load_tree(p), fam_dir), capture_output=True, text=True,
                              encoding="utf-8", check=True).stdout
        if args.check:
            if not out.is_file() or out.read_text(encoding="utf-8") != text:
                print(f"STALE {out} -- run python3 scripts/gen_power_tree.py", file=sys.stderr)
                stale += 1
            continue
        out.write_text(text, encoding="utf-8", newline="")
        print(f"wrote {out}")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
