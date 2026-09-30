#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Cross-check a chip driver's hand-written register tables against its
`metadata/chips/<part>.yaml` manifest (issue #2347).

`metadata/chips/act8760.yaml`, `da9292.yaml` and `tps628640.yaml` are the
single source of truth for each PMIC's register map and raw-write policy
(`write: allow|guarded|deny`).  The C drivers under `chips/<part>/` restate
the same addresses -- `chips/act8760/act8760.c`'s `rail_table[]` /
`raw_write_allow[]`, `chips/da9292/da9292.c`'s per-channel `#define`s and
`DA9292_RAW_WRITE_FIRST/LAST` window, `chips/tps628640/tps628640.c`'s
`TPS628640_REG_*` channel registers -- for readability; nothing generated
them, and until this gate nothing compared them.  Two failure modes that
would previously build clean and pass every test:

* A digit slip in either table (`vset0_reg: 0x4A` vs the driver's `0x42`)
  programs the wrong register on a live PMIC with no error, no test
  failure, no gate output.
* `metadata/chips/<part>.yaml` flipping a register's `write:` from
  `guarded`/`deny` to `allow` (or vice versa) changes nothing in the
  driver, silently making the header's "the allow-list is the enforcement"
  claim (`include/alp/chips/act8760.h`) a lie.

This gate parses the driver's raw-write allow surface -- a literal
`static const uint8_t *allow*[] = {...}` array (act8760's
`raw_write_allow[]`), or a `<PREFIX>_RAW_WRITE_FIRST`/`_RAW_WRITE_LAST`
inclusive register window (da9292's `da9292_write_reg()`) -- and diffs it
against the manifest's `write: allow` addresses.  It also hand-evaluates
act8760's `BUCK_R`/`BUCK_R1`/`BUCK_F`/`LDO` rail-table macros and da9292 /
tps628640's per-channel register `#define`s, and diffs the resulting
per-rail/per-channel register addresses against `rails:`/`channels:` in
the manifest.

A chip with no `write:` key anywhere in its `register_table:` (e.g.
`clk_5l35023b.yaml`) makes no raw-write-policy enforcement claim and is
skipped for that half of the check.

ponytail: this gate knows exactly three chips -- act8760, da9292,
tps628640 -- the ones issue #2347 named, whose driver source it hand-parses
per-chip.  It is not a generic C-table-vs-YAML differ: extending it to a
fourth chip needs a new extractor function here, not a config knob.  Worth
it because the alternative (a fully generic C-macro evaluator) is a much
bigger, fussier piece of code for three known offenders.  chip-v1.schema.json
typing `rails[]`/`channels[]`/`register_table[]` items (also proposed in
#2347) is a separate, wider change (touches all 88 chip manifests) and is
not this gate's job.

Exit codes:
* 0  -- every checked chip's driver tables agree with its manifest.
* 1  -- one or more mismatches.

Run locally:

    python3 scripts/check_chip_reg_parity.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

_DEFINE_HEX_RE = re.compile(
    r"^#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+)u?\b", re.MULTILINE
)
_DEFINE_SHIFT_RE = re.compile(
    r"^#define\s+(\w+)\s+\(1u\s*<<\s*(\d+)\)", re.MULTILINE
)


def _parse_defines(c_text: str) -> dict[str, int]:
    """Return {MACRO_NAME: int_value} for every hex or `(1u << n)` #define."""
    out: dict[str, int] = {}
    for m in _DEFINE_HEX_RE.finditer(c_text):
        out[m.group(1)] = int(m.group(2), 16)
    for m in _DEFINE_SHIFT_RE.finditer(c_text):
        out[m.group(1)] = 1 << int(m.group(2))
    return out


def _bit_index(value: int) -> int | None:
    """Inverse of `1 << n`; None if value is not a single bit."""
    if value <= 0 or (value & (value - 1)) != 0:
        return None
    return value.bit_length() - 1


def _driver_raw_write_allow(
    c_text: str, defines: dict[str, int]
) -> set[int] | None:
    """The set of addresses `<part>_write_reg()` will pass through, or
    None if the source matches neither known pattern (array literal or
    FIRST/LAST window) -- callers treat that as "no raw-write surface"."""
    m = re.search(
        r"static\s+const\s+uint8_t\s+\w*[Aa]llow\w*\s*\[\s*\]\s*=\s*\{([^}]*)\}",
        c_text,
    )
    if m:
        return {int(tok, 16) for tok in re.findall(r"0[xX][0-9A-Fa-f]+", m.group(1))}

    mf = re.search(r"#define\s+\w+_RAW_WRITE_FIRST\s+(\w+)", c_text)
    ml = re.search(r"#define\s+\w+_RAW_WRITE_LAST\s+(\w+)", c_text)
    if mf and ml:
        first = defines.get(mf.group(1))
        last = defines.get(ml.group(1))
        if first is None or last is None:
            return None
        return {
            v
            for name, v in defines.items()
            if "_REG_" in name and first <= v <= last
        }

    return set()


def _check_write_policy(
    chip: str, c_text: str, defines: dict[str, int], register_table: list[dict]
) -> list[str]:
    rows_with_write = [r for r in register_table if "write" in r]
    if not rows_with_write:
        return []  # no enforcement claim to check (e.g. clk_5l35023b)

    yaml_allow = {r["addr"] for r in rows_with_write if r.get("write") == "allow"}
    driver_allow = _driver_raw_write_allow(c_text, defines)
    if driver_allow is None:
        return [
            f"chips/{chip}/{chip}.c: RAW_WRITE_FIRST/LAST references a macro "
            f"this gate cannot resolve -- update the parser in "
            f"scripts/check_chip_reg_parity.py"
        ]

    problems = []
    missing = sorted(yaml_allow - driver_allow)
    extra = sorted(driver_allow - yaml_allow)
    if missing:
        problems.append(
            f"metadata/chips/{chip}.yaml: write: allow at "
            f"{[hex(a) for a in missing]} but chips/{chip}/{chip}.c's "
            f"raw-write allow surface does not include it"
        )
    if extra:
        problems.append(
            f"chips/{chip}/{chip}.c: raw-write allow surface permits "
            f"{[hex(a) for a in extra]}, but metadata/chips/{chip}.yaml does "
            f"not mark it write: allow"
        )
    return problems


# --- act8760: rail_table[] macro evaluation -------------------------------

_ACT8760_RAIL_ROW_RE = re.compile(
    r"\[ACT8760_RAIL_(\w+)\]\s*=\s*(BUCK_R1|BUCK_R|BUCK_F|LDO)\(([^)]*)\)"
)


def _hex_arg(tok: str) -> int | None:
    tok = tok.strip()
    m = re.match(r"^0[xX][0-9A-Fa-f]+", tok)
    return int(m.group(0), 16) if m else None


def _act8760_rail_regs(macro: str, args: list[str]) -> dict:
    """Reproduce the field arithmetic of act8760.c's BUCK_R/BUCK_R1/BUCK_F/LDO
    macros: status_reg=b, vset_reg=b+offset, en_reg=b+offset, range_reg
    (None if the rail has a fixed range)."""
    if macro == "BUCK_R":
        b = _hex_arg(args[1])
        return {
            "status_reg": b,
            "vset0_reg": b + 2,
            "enable_reg": b + 4,
            "range_reg": b + 6,
        }
    if macro == "BUCK_R1":
        b = _hex_arg(args[0])
        return {
            "status_reg": b,
            "vset0_reg": b + 2,
            "enable_reg": b + 4,
            "range_reg": b + 1,
        }
    if macro == "BUCK_F":
        b = _hex_arg(args[0])
        return {
            "status_reg": b,
            "vset0_reg": b + 2,
            "enable_reg": b + 4,
            "range_reg": None,
        }
    if macro == "LDO":
        b = _hex_arg(args[0])
        return {
            "status_reg": b,
            "vset0_reg": b + 1,
            "enable_reg": b + 2,
            "range_reg": b + 1,
        }
    raise ValueError(macro)


def _check_act8760_rails(c_text: str, rails: list[dict]) -> list[str]:
    m = re.search(
        r"rail_table\[ACT8760_RAIL_COUNT\]\s*=\s*\{(.*?)\n\};", c_text, re.DOTALL
    )
    if not m:
        return [
            "chips/act8760/act8760.c: rail_table[] not found in the shape this "
            "gate expects -- update scripts/check_chip_reg_parity.py"
        ]
    body = m.group(1)

    driver_rails: dict[str, dict] = {}
    for row in _ACT8760_RAIL_ROW_RE.finditer(body):
        label, macro, argtext = row.group(1), row.group(2), row.group(3)
        rail_id = label.lower()
        driver_rails[rail_id] = _act8760_rail_regs(macro, argtext.split(","))

    problems = []
    yaml_rails = {r["id"]: r for r in rails}
    for rail_id, yaml_row in yaml_rails.items():
        driver_row = driver_rails.get(rail_id)
        if driver_row is None:
            problems.append(
                f"metadata/chips/act8760.yaml: rails[] has {rail_id!r}, but "
                f"chips/act8760/act8760.c's rail_table[] has no "
                f"ACT8760_RAIL_{rail_id.upper()} entry"
            )
            continue
        for field in ("status_reg", "vset0_reg", "enable_reg", "range_reg"):
            yaml_val = yaml_row.get(field)
            driver_val = driver_row.get(field)
            if yaml_val != driver_val:
                problems.append(
                    f"metadata/chips/act8760.yaml: rails[{rail_id!r}].{field} = "
                    f"{yaml_val!r} but chips/act8760/act8760.c's rail_table[] "
                    f"computes {driver_val!r} for ACT8760_RAIL_{rail_id.upper()}"
                )
    for rail_id in driver_rails:
        if rail_id not in yaml_rails:
            problems.append(
                f"chips/act8760/act8760.c: rail_table[] has "
                f"ACT8760_RAIL_{rail_id.upper()}, but metadata/chips/act8760.yaml's "
                f"rails[] has no {rail_id!r} entry"
            )
    return problems


# --- da9292 / tps628640: named-#define channel registers -----------------


def _check_named_channel_regs(
    chip: str,
    defines: dict[str, int],
    channels: list[dict],
    field_to_macro: dict,
) -> list[str]:
    """field_to_macro[field] is either a macro-name string, or a callable
    ch_id -> macro-name, for fields that vary per channel."""
    problems = []
    for ch in channels:
        ch_id = ch["id"]
        for field, macro_spec in field_to_macro.items():
            if field not in ch:
                continue
            macro = macro_spec(ch_id) if callable(macro_spec) else macro_spec
            if macro not in defines:
                problems.append(
                    f"chips/{chip}/{chip}.c: no #define {macro} to check "
                    f"metadata/chips/{chip}.yaml's channels[{ch_id!r}].{field} "
                    f"against"
                )
                continue
            driver_val = defines[macro]
            yaml_val = ch[field]
            if field.endswith("_bit"):
                driver_val = _bit_index(driver_val)
            if driver_val != yaml_val:
                problems.append(
                    f"metadata/chips/{chip}.yaml: channels[{ch_id!r}].{field} = "
                    f"{yaml_val!r} but chips/{chip}/{chip}.c's {macro} is "
                    f"{driver_val!r}"
                )
    return problems


def _da9292_field_macros() -> dict:
    def vsel_lo(ch_id: str) -> str:
        return f"DA9292_REG_PMC_VOUT_{ch_id.upper()}_00"

    def vsel_hi(ch_id: str) -> str:
        return f"DA9292_REG_PMC_VOUT_{ch_id.upper()}_01"

    def bit_macro(field: str):
        def _m(ch_id: str) -> str:
            return f"DA9292_CTRL01_{ch_id.upper()}_{field}"

        return _m

    return {
        "ctrl_reg": "DA9292_REG_PMC_CTRL_01",
        "vsel_lo_reg": vsel_lo,
        "vsel_hi_reg": vsel_hi,
        "en_bit": bit_macro("EN"),
        "vsel_bit": bit_macro("VSEL"),
        "dis_pd_bit": bit_macro("DIS_PD"),
        "vstep_bit": bit_macro("VSTEP"),
    }


def _tps628640_field_macros() -> dict:
    return {
        "vout_reg": lambda ch_id: f"TPS628640_REG_{ch_id.upper()}",
    }


def _check_chip(root: Path, chip: str) -> list[str]:
    yaml_path = root / "metadata" / "chips" / f"{chip}.yaml"
    c_path = root / "chips" / chip / f"{chip}.c"
    if not yaml_path.is_file() or not c_path.is_file():
        return [f"{chip}: missing {yaml_path} or {c_path}"]

    manifest = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    c_text = c_path.read_text(encoding="utf-8")
    # Register #defines can live in the public header (tps628640.h) instead
    # of the .c file (act8760.c, da9292.c keep theirs private); concatenate
    # so `_parse_defines` sees whichever the chip uses.
    header_path = root / "include" / "alp" / "chips" / f"{chip}.h"
    header_text = header_path.read_text(encoding="utf-8") if header_path.is_file() else ""
    defines = _parse_defines(c_text + "\n" + header_text)

    problems = _check_write_policy(
        chip, c_text, defines, manifest.get("register_table", []) or []
    )

    if chip == "act8760":
        problems += _check_act8760_rails(c_text, manifest.get("rails", []) or [])
    elif chip == "da9292":
        problems += _check_named_channel_regs(
            chip, defines, manifest.get("channels", []) or [], _da9292_field_macros()
        )
    elif chip == "tps628640":
        problems += _check_named_channel_regs(
            chip,
            defines,
            manifest.get("channels", []) or [],
            _tps628640_field_macros(),
        )

    return problems


CHIPS = ("act8760", "da9292", "tps628640")


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    for chip in CHIPS:
        problems.extend(_check_chip(root, chip))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=ROOT, help="repository root to check"
    )
    args = parser.parse_args()

    problems = find_problems(args.root)
    if problems:
        for p in problems:
            print(f"chip-reg-parity: {p}", file=sys.stderr)
        return 1
    print(
        "OK: act8760/da9292/tps628640 driver register tables agree with "
        "their metadata/chips/*.yaml manifests."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
