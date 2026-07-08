#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
E1M board-route <-> SoM capability-table end-to-end gate.

Every `e1m: E1M_<class><N>` route a board preset declares must resolve to
real E1M-pad functions the SoM actually exposes -- i.e. to entries in the
generated capability table `metadata/pinmux/<family>.yaml`.  This turns the
board<->module integration from "hopefully consistent" into a CI lint: a
carrier that wires a pad the module doesn't back fails here, not on the
bench.

The `E1M_<class><N>` -> `e1m_function` alias is NOT hand-maintained data --
it falls straight out of the E1M edge-connector pin names the SoM netlist
already defines (`E1M_ADC<N>` -> `ANA_S<N>`, `E1M_I2C<N>` -> the SCL/SDA
pair, `E1M_SPI1` -> the SPI1_* set, ...).  The rules below encode exactly
that, with no invented mappings.

Routes the SoM legitimately does NOT back (a pad left open on the module,
or carrier-only) are listed per (board, family) in KNOWN_UNBACKED with a
reason -- a RATCHET: a new unbacked route fails the gate, and the list can
only shrink as the carrier or the module catches up.

Run locally:

    python3 scripts/check_e1m_route_capability.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOARDS = REPO / "metadata" / "boards"
PINMUX = REPO / "metadata" / "pinmux"

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("check_e1m_route_capability: PyYAML is required.")

# (board preset, capability-table family) pairs to check.
CHECKS = [("e1m-evk", "aen")]

# Routes a board wires that the SoM does NOT back -- pad left open on the
# module or carrier-only.  BACKLOG ONLY: shrink by fixing the board preset
# or the module; a new unbacked route must fail, not be allowlisted away.
KNOWN_UNBACKED: dict[tuple[str, str], dict[str, str]] = {
    ("e1m-evk", "aen"): {
        "E1M_GPIO_IO21": "2626-R2: IO21 unrouted on the module (GPIO_30 moved to IO8).",
        "E1M_GPIO_IO22": "2626-R2: left open on the module (CC3501E CS+IRQ took the Alif pins); EVK wires it to PCIe MUX_PD.",
        "E1M_GPIO_IO23": "2626-R2: left open on the module; EVK wires it to PCIe MUX_SEL.",
    },
}


def _expected_functions(ref: str) -> list[str] | None:
    """E1M_<class><N> -> the e1m_function name(s) it must resolve to, or
    None when the class is unknown (a hard failure, not a silent pass)."""
    m = re.fullmatch(r"E1M_(.+?)(\d*)", ref)
    if not m:
        return None
    cls, n = m.group(1), m.group(2)
    # GPIO pads: E1M_GPIO_IO<N> -> IO<N>; E1M_GPIO_PWM<N> -> PWM<N> (the
    # PWM pad's secondary GPIO function).
    if cls == "GPIO_IO":
        return [f"IO{n}"]
    if cls == "GPIO_PWM":
        return [f"PWM{n}"]
    if cls == "PWM":
        return [f"PWM{n}"]
    if cls == "DAC":
        return [f"DAC{n}"]
    if cls == "ADC":                      # connector names analog pads ANA_S<N>
        return [f"ANA_S{n}"]
    if cls == "UART":
        return [f"UART{n}_RX", f"UART{n}_TX"]
    if cls == "I2C":
        return [f"I2C{n}_SCL", f"I2C{n}_SDA"]
    if cls == "I2S":
        return [f"I2S{n}_SCLK", f"I2S{n}_SDI", f"I2S{n}_SDO", f"I2S{n}_WS"]
    if cls == "I3C":                      # single I3C bus on the connector
        return ["I3C_SCL", "I3C_SDA"]
    if cls == "SPI":
        return [f"SPI{n}_MISO", f"SPI{n}_MOSI", f"SPI{n}_SCLK",
                f"SPI{n}_CS0", f"SPI{n}_CS1"]
    if cls == "ENC":
        return [f"ENC{n}_X", f"ENC{n}_Y"]
    if cls == "CAN":
        return [f"__PREFIX__CAN{n}"]      # match any CAN<N>* (CAN0H/CAN0_TX, ...)
    return None


def _resolves(ref: str, funcs: set[str]) -> tuple[bool, list[str]]:
    """(resolved?, missing) -- resolved when every expected function is in
    the table.  Returns ([ref], []) shape via the missing list."""
    expected = _expected_functions(ref)
    if expected is None:
        return False, [f"<unknown class for {ref}>"]
    if expected and expected[0].startswith("__PREFIX__"):
        prefix = expected[0][len("__PREFIX__"):]
        hit = [f for f in funcs if f.startswith(prefix)]
        return (bool(hit), [] if hit else [f"{prefix}*"])
    missing = [e for e in expected if e not in funcs]
    return (not missing, missing)


def _board_refs(board: str) -> list[str]:
    doc = yaml.safe_load((BOARDS / f"{board}.yaml").read_text(encoding="utf-8"))
    refs: set[str] = set()

    def walk(node):
        if isinstance(node, dict):
            v = node.get("e1m")
            if isinstance(v, str) and v.startswith("E1M_") and not v.startswith("E1M_X_"):
                refs.add(v)
            for x in node.values():
                walk(x)
        elif isinstance(node, list):
            for x in node:
                walk(x)

    walk(doc)
    return sorted(refs)


def _table_functions(family: str) -> set[str]:
    doc = yaml.safe_load((PINMUX / f"{family}.yaml").read_text(encoding="utf-8"))
    return {p["e1m_function"] for p in doc.get("pads", [])}


def main() -> int:
    errors: list[str] = []
    for board, family in CHECKS:
        funcs = _table_functions(family)
        allow = KNOWN_UNBACKED.get((board, family), {})
        refs = _board_refs(board)
        backed = stale_allow = 0
        for ref in refs:
            ok, missing = _resolves(ref, funcs)
            if ok:
                backed += 1
                if ref in allow:
                    errors.append(f"{board}: {ref} is now backed by {family} -- "
                                  f"remove it from KNOWN_UNBACKED (stale ratchet).")
                    stale_allow += 1
            elif ref in allow:
                print(f"OPEN {board}: {ref} -- {allow[ref]}")
            else:
                errors.append(f"{board}: {ref} resolves to {missing} which the "
                              f"{family} SoM capability table does not back.")
        print(f"\n{board} <-> {family}: {backed}/{len(refs)} routes backed, "
              f"{len(allow)} known-unbacked.\n")

    if errors:
        for e in errors:
            print(f"e1m-route-capability: {e}", file=sys.stderr)
        print(f"\ncheck_e1m_route_capability: {len(errors)} issue(s).", file=sys.stderr)
        return 1
    print("check_e1m_route_capability: every board route resolves to the SoM "
          "capability table (or is a documented open pad).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
