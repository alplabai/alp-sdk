#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Chip register-table parity gate (issue #2347).

The V2N power-chip drivers hand-copy register facts that
metadata/chips/<part>.yaml also records, and nothing compared the two: a
one-digit slip (`vset0_reg: 0x4A` vs the C table's `0x42`) makes a driver
program the wrong register on a live PMIC with no build error, no test
failure and no gate output.  `check_chip_manifest_parity.py` only checks that
driver and manifest both EXIST; this gate compares their CONTENTS:

  act8760    chips/act8760/act8760.c `rail_table[]` (expanded through the
             file's own BUCK_R/BUCK_R1/BUCK_F/LDO macros) vs `rails[]`
             (slave, status/VSET0/ON registers, range selector, VSET mask,
             per-range base/step), and `raw_write_allow[]` vs the ADD1
             `register_table[]` rows marked `write: allow`.
  da9292     chips/da9292/da9292.c `DA9292_REG_*` vs `register_table[]` by
             name, the `DA9292_CTRL01_*` bits and VOUT registers vs
             `channels[]`, and the raw-write window vs the `write: allow` rows.
  tps628640  include/alp/chips/tps628640.h `TPS628640_REG_*` vs
             `register_table[]` addresses, and `TPS628640_VOUT_*` / REG_VOUTn vs
             `channels[]`.

A generator cannot replace the hand-written C tables (they are written for
readability), so this is a checker; failing is the point.  Exit 1 with one
line per mismatch naming the file, the field and both values.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent

_NUM = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\b")


def _join_continuations(text: str) -> str:
    return re.sub(r"\\\r?\n", " ", text)


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _defines(text: str) -> tuple[dict[str, str], dict[str, tuple[list[str], str]]]:
    """Object-like and function-like #defines of one C file."""
    obj: dict[str, str] = {}
    fn: dict[str, tuple[list[str], str]] = {}
    for line in _join_continuations(_strip_comments(text)).splitlines():
        m = re.match(r"\s*#\s*define\s+(\w+)\(([^)]*)\)\s*(.*)$", line)
        if m:
            fn[m[1]] = ([p.strip() for p in m[2].split(",") if p.strip()], m[3].strip())
            continue
        m = re.match(r"\s*#\s*define\s+(\w+)\s+(.+)$", line)
        if m:
            obj[m[1]] = m[2].strip()
    return obj, fn


def _split_args(s: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def _expand(expr: str, obj: dict, fn: dict, depth: int = 0) -> str:
    """Tiny C preprocessor: object- and function-like macro expansion."""
    if depth > 20:
        raise ValueError(f"macro expansion too deep: {expr[:80]}")
    for name, (params, body) in fn.items():
        while True:
            m = re.search(rf"\b{name}\s*\(", expr)
            if not m:
                break
            i, depth_p = m.end(), 1
            while depth_p:
                depth_p += {"(": 1, ")": -1}.get(expr[i], 0)
                i += 1
            args = _split_args(expr[m.end():i - 1])
            sub = body
            for p, a in zip(params, args):
                sub = re.sub(rf"\b{p}\b", f"({a})", sub)
            expr = expr[:m.start()] + sub + expr[i:]
    changed = True
    while changed:
        changed = False
        for name, body in obj.items():
            new = re.sub(rf"\b{name}\b", f"({body})", expr)
            if new != expr:
                expr, changed = new, True
    return _expand(expr, obj, fn, depth + 1) if any(re.search(rf"\b{n}\s*\(", expr) for n in fn) else expr


def _eval(expr: str, names: dict[str, int] | None = None):
    """Evaluate an expanded C integer / brace-initializer expression."""
    py = _NUM.sub(lambda m: str(int(m[1], 0)), expr).replace("{", "[").replace("}", "]")
    py = re.sub(r"\btrue\b", "1", re.sub(r"\bfalse\b", "0", py))
    return eval(py, {"__builtins__": {}}, dict(names or {}))  # noqa: S307 -- repo-local C text


def _hex(v) -> str:
    return f"{v:#04x}" if isinstance(v, int) else repr(v)


def _manifest(root: Path, chip: str) -> dict:
    return yaml.safe_load((root / "metadata" / "chips" / f"{chip}.yaml").read_text(encoding="utf-8"))


def _allow_addrs(manifest: dict, slave: str | None = None) -> set[int]:
    out: set[int] = set()
    for row in manifest.get("register_table") or []:
        if row.get("write") != "allow" or (slave and row.get("slave") != slave):
            continue
        if "addr_range" in row:
            lo, hi = row["addr_range"]
            out |= set(range(lo, hi + 1))
        else:
            out.add(row["addr"])
    return out


# --------------------------------------------------------------------------
# ACT88760
# --------------------------------------------------------------------------

_ACT_FIELDS = ("page", "status_reg", "vset_reg", "en_reg", "range_reg", "range_mask",
               "fixed_range", "vset_mask", "is_buck", "banked", "scale")


def _check_act8760(root: Path) -> list[str]:
    src_path = root / "chips" / "act8760" / "act8760.c"
    hdr = (root / "include" / "alp" / "chips" / "act8760.h").read_text(encoding="utf-8")
    src = src_path.read_text(encoding="utf-8")
    rel = src_path.relative_to(root).as_posix()
    obj, fn = _defines(src)
    names = {"ACT8760_PAGE_SYSTEM": 0, "ACT8760_PAGE_AUX": 1}
    m = re.search(r"#define\s+ACT8760_TILE_ON\s+(0x[0-9A-Fa-f]+)u?", src)
    tile_on = int(m[1], 16) if m else None
    body = re.search(r"rail_table\[ACT8760_RAIL_COUNT\]\s*=\s*\{(.*?)\n\};", _strip_comments(src), re.S)
    if not body:
        return [f"{rel}: rail_table[] not found -- update scripts/check_chip_reg_parity.py"]
    c_rails = {}
    for idx, init in re.findall(r"\[ACT8760_RAIL_(\w+)\]\s*=\s*(\w+\([^\n]*\))\s*,", body[1]):
        vals = _eval(_expand(init, obj, fn), names)
        c_rails[idx.lower()] = dict(zip(_ACT_FIELDS, vals))
    if "ACT8760_RAIL_COUNT" not in hdr:
        return [f"include/alp/chips/act8760.h: ACT8760_RAIL_COUNT missing"]

    man = _manifest(root, "act8760")
    errs: list[str] = []
    m_rails = {r["id"]: r for r in man.get("rails") or []}
    for rid in sorted(set(m_rails) ^ set(c_rails)):
        where = "metadata only" if rid in m_rails else f"{rel} only"
        errs.append(f"act8760 rail {rid}: {where} -- rail_table[] and rails[] must list the same rails")
    for rid in sorted(set(m_rails) & set(c_rails)):
        y, c = m_rails[rid], c_rails[rid]
        want_page = {"add1": 0, "add2": 1}.get(y.get("slave"))

        def diff(field, yv, cv):
            if yv != cv:
                errs.append(f"act8760 rail {rid}: {field} metadata {_hex(yv)} != {rel} {_hex(cv)}")

        diff("slave/page", want_page, c["page"])
        diff("status_reg", y.get("status_reg"), c["status_reg"])
        diff("vset0_reg", y.get("vset0_reg"), c["vset_reg"])
        diff("enable_reg", y.get("enable_reg"), c["en_reg"])
        diff("vset_mask", y.get("vset_mask"), c["vset_mask"])
        diff("kind", y.get("kind"), "buck" if c["is_buck"] else "ldo")
        if tile_on is not None and y.get("enable_bit") is not None:
            diff("enable_bit", 1 << y["enable_bit"], tile_on)
        if y.get("range_reg") is None:
            if c["range_mask"]:
                errs.append(f"act8760 rail {rid}: metadata has no range selector but {rel} "
                            f"reads range at {_hex(c['range_reg'])} mask {_hex(c['range_mask'])}")
        else:
            diff("range_reg", y["range_reg"], c["range_reg"])
            diff("range_bit mask", 1 << y["range_bit"], c["range_mask"])
        for r in y.get("ranges") or []:
            got = list(c["scale"][r["range"]])
            diff(f"range {r['range']} base_mv/step_uv", [r["base_mv"], r["step_uv"]], got)
        if y.get("range_reg") is None and len(y.get("ranges") or []) == 1:
            diff("fixed range", y["ranges"][0]["range"], c["fixed_range"])

    m_allow = re.search(r"raw_write_allow\[\]\s*=\s*\{([^}]*)\}", src)
    c_allow = {int(x, 0) for x in re.findall(r"0[xX][0-9A-Fa-f]+", m_allow[1])} if m_allow else set()
    y_allow = _allow_addrs(man, "add1")
    for a in sorted(y_allow - c_allow):
        errs.append(f"act8760: {_hex(a)} is `write: allow` in metadata but not in {rel} raw_write_allow[]")
    for a in sorted(c_allow - y_allow):
        errs.append(f"act8760: {rel} raw_write_allow[] allows {_hex(a)}, which metadata does not mark `write: allow`")
    return errs


# --------------------------------------------------------------------------
# DA9292
# --------------------------------------------------------------------------

def _check_da9292(root: Path) -> list[str]:
    src_path = root / "chips" / "da9292" / "da9292.c"
    rel = src_path.relative_to(root).as_posix()
    obj, fn = _defines(src_path.read_text(encoding="utf-8"))
    val = lambda n: _eval(_expand(n, obj, fn)) if n in obj else None  # noqa: E731
    man = _manifest(root, "da9292")
    errs: list[str] = []
    rows = {r["name"]: r for r in man.get("register_table") or [] if "name" in r and "addr" in r}
    regs = {n[len("DA9292_REG_"):]: val(n) for n in obj if n.startswith("DA9292_REG_")}
    for name in sorted(set(rows) | set(regs)):
        if name not in rows:
            errs.append(f"da9292: {rel} defines DA9292_REG_{name} but register_table[] has no `{name}` row")
        elif name not in regs:
            errs.append(f"da9292: register_table[] row `{name}` has no DA9292_REG_{name} in {rel}")
        elif rows[name]["addr"] != regs[name]:
            errs.append(f"da9292: {name} metadata {_hex(rows[name]['addr'])} != {rel} {_hex(regs[name])}")
    for ch in man.get("channels") or []:
        n = ch["id"][-1]
        checks = {
            "ctrl_reg": val("DA9292_REG_PMC_CTRL_01"),
            "vsel_lo_reg": val(f"DA9292_REG_PMC_VOUT_CH{n}_00"),
            "vsel_hi_reg": val(f"DA9292_REG_PMC_VOUT_CH{n}_01"),
        }
        for bit in ("en", "vsel", "dis_pd", "vstep"):
            v = val(f"DA9292_CTRL01_CH{n}_{bit.upper()}")
            checks[f"{bit}_bit"] = v.bit_length() - 1 if v else None
        for field, cv in checks.items():
            if ch.get(field) != cv:
                errs.append(f"da9292 {ch['id']}: {field} metadata {_hex(ch.get(field))} != {rel} {_hex(cv)}")
    lo, hi = val("DA9292_RAW_WRITE_FIRST"), val("DA9292_RAW_WRITE_LAST")
    c_allow = set(range(lo, hi + 1)) if lo is not None and hi is not None else set()
    y_allow = _allow_addrs(man)
    if c_allow != y_allow:
        errs.append(f"da9292: {rel} raw-write window {sorted(map(_hex, c_allow))} != metadata "
                    f"`write: allow` rows {sorted(map(_hex, y_allow))}")
    return errs


# --------------------------------------------------------------------------
# TPS628640
# --------------------------------------------------------------------------

def _check_tps628640(root: Path) -> list[str]:
    hdr_path = root / "include" / "alp" / "chips" / "tps628640.h"
    rel = hdr_path.relative_to(root).as_posix()
    obj, fn = _defines(hdr_path.read_text(encoding="utf-8"))
    val = lambda n: _eval(_expand(n, obj, fn)) if n in obj else None  # noqa: E731
    man = _manifest(root, "tps628640")
    errs: list[str] = []
    c_regs = {val(n) for n in obj if n.startswith("TPS628640_REG_")}
    y_regs = {r["addr"] for r in man.get("register_table") or [] if "addr" in r}
    if c_regs != y_regs:
        errs.append(f"tps628640: {rel} TPS628640_REG_* {sorted(map(_hex, c_regs))} != metadata "
                    f"register_table[] {sorted(map(_hex, y_regs))}")
    for ch in man.get("channels") or []:
        n = ch["id"][-1]
        checks = {"vout_reg": val(f"TPS628640_REG_VOUT{n}"), "base_mv": val("TPS628640_VOUT_BASE_MV"),
                  "abs_min_mv": val("TPS628640_VOUT_BASE_MV"), "abs_max_mv": val("TPS628640_VOUT_MAX_MV"),
                  "step_mv": val("TPS628640_VOUT_STEP_MV")}
        for field, cv in checks.items():
            if ch.get(field) != cv:
                errs.append(f"tps628640 {ch['id']}: {field} metadata {_hex(ch.get(field))} != {rel} {_hex(cv)}")
    return errs


CHECKS = {"act8760": _check_act8760, "da9292": _check_da9292, "tps628640": _check_tps628640}


def find_problems(root: Path = REPO) -> list[str]:
    errs: list[str] = []
    for chip, check in CHECKS.items():
        try:
            errs += check(root)
        except (OSError, KeyError, ValueError, SyntaxError, TypeError) as e:
            errs.append(f"{chip}: could not parse the driver table ({type(e).__name__}: {e}) -- "
                        f"update scripts/check_chip_reg_parity.py alongside the driver")
    return errs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", type=Path, default=REPO)
    a = ap.parse_args(argv)
    errs = find_problems(a.root)
    for e in errs:
        print(f"FAIL {e}")
    if errs:
        print(f"check-chip-reg-parity: {len(errs)} mismatch(es) between chips/ and metadata/chips/ "
              f"-- fix whichever side is wrong (the datasheet decides)")
        return 1
    print(f"check-chip-reg-parity: OK ({', '.join(CHECKS)} tables match metadata/chips/)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
