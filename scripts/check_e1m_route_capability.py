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
CHECKS = [("e1m-evk", "aen"), ("e1m-x-evk", "v2n")]

# Routes a board wires that the SoM does NOT back -- pad left open on the
# module or carrier-only.  BACKLOG ONLY: shrink by fixing the board preset
# or the module; a new unbacked route must fail, not be allowlisted away.
KNOWN_UNBACKED: dict[tuple[str, str], dict[str, str]] = {
    ("e1m-evk", "aen"): {
        "E1M_GPIO_IO21": "2626-R2: IO21 unrouted on the module (GPIO_30 moved to IO8).",
        "E1M_GPIO_IO22": "2626-R2: left open on the module (CC3501E CS+IRQ took the Alif pins); EVK wires it to PCIe MUX_PD.",
        "E1M_GPIO_IO23": "2626-R2: left open on the module; EVK wires it to PCIe MUX_SEL.",
    },
    ("e1m-x-evk", "v2n"): {
        "E1M_X_GPIO_IO0": "X-EVK PCIe lane-mux power-down; carrier-side control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO1": "X-EVK PCIe lane-mux select; carrier-side control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO2": "X-EVK PCIe-slot I2C-mux enable; carrier-side control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO4": "X-EVK I2S path-mux enable; carrier-side control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO5": "X-EVK I2S path-mux select; carrier-side control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO6": "M.2 E-key UART wake sideband; carrier/M.2 control, not a V2N-backed E1M-X GPIO.",
        "E1M_X_GPIO_IO15": "LCD1 power enable is carrier-pulled on V2N X-EVK; no firmware-controlled V2N E1M-X GPIO route.",
        "E1M_X_GPIO_IO17": "Display-2 touch interrupt is wired for future dual-DSI SoMs; V2N/V2M do not expose this sideband.",
        "E1M_X_GPIO_IO18": "Camera-0 power enable is a carrier sideband not exposed as a current V2N E1M-X GPIO route.",
        "E1M_X_GPIO_IO19": "Display-2 touch reset is wired for future dual-DSI SoMs; V2N/V2M do not expose this sideband.",
        "E1M_X_GPIO_IO20": "Camera-0 reset is a carrier sideband not exposed as a current V2N E1M-X GPIO route.",
        "E1M_X_GPIO_IO21": "Display-2 reset is wired for future dual-DSI SoMs; V2N/V2M have only one DSI output.",
        "E1M_X_GPIO_IO22": "Display-2 power enable is wired for future dual-DSI SoMs; V2N/V2M have only one DSI output.",
        "E1M_X_I2S1_SCLK": "Amp shutdown uses an otherwise-unused I2S1 pad as a GPIO sideband; V2N pinmux table has no backed I2S1 GPIO function.",
        "E1M_X_I2S1_SDI": "Amp fault uses an otherwise-unused I2S1 pad as a GPIO sideband; V2N pinmux table has no backed I2S1 GPIO function.",
    },
}


def _v2n_function_aliases(silicon_peripheral: str) -> list[str]:
    """Aliases for V2N pad-first rows whose E1M function is not in the TSV.

    The V2N Renesas map is pad-first, so direct RIIC/UART/RSPI/SSIU/CAN rows
    land in `silicon_peripheral` with `e1m_function: TBD`.  These aliases keep
    the checker from treating present direct buses as unbacked while still
    failing carrier sidebands that have no V2N route at all.
    """
    m = re.fullmatch(r"RIIC(\d+)_SCL\d+", silicon_peripheral)
    if m:
        return [f"I2C{m.group(1)}.SCL"]
    m = re.fullmatch(r"RIIC(\d+)_SDA\d+", silicon_peripheral)
    if m:
        return [f"I2C{m.group(1)}.SDA"]

    m = re.fullmatch(r"UART(\d+)_RXD\d+", silicon_peripheral)
    if m:
        return [f"UART{m.group(1)}_RX"]
    m = re.fullmatch(r"UART(\d+)_TXD\d+", silicon_peripheral)
    if m:
        return [f"UART{m.group(1)}_TX"]

    rspi0_to_spi1 = {
        "RSPI0_MISOA": "SPI1_MISO",
        "RSPI0_MOSIA": "SPI1_MOSI",
        "RSPI0_RSPCKA": "SPI1_SCLK",
        "RSPI0_SSLA0": "SPI1_CS0",
        "RSPI0_SSLA1": "SPI1_CS1",
    }
    if silicon_peripheral in rspi0_to_spi1:
        return [rspi0_to_spi1[silicon_peripheral]]

    ssiu_to_i2s0 = {
        "SSIU1_SSI1_SCK": "I2S0_SCLK",
        "SSIU1_SSI1_WS": "I2S0_WS",
        "SSIU1_SSI1_SDATA": "I2S0_SDO",
        "SSIU2_SSI2_SDATA": "I2S0_SDI",
    }
    if silicon_peripheral in ssiu_to_i2s0:
        return [ssiu_to_i2s0[silicon_peripheral]]

    canfd_to_e1m = {
        "CANFD2_CRX2": "CAN0_RX",
        "CANFD2_CTX2": "CAN0_TX",
        "CANFD3_CRX3": "CAN1_RX",
        "CANFD3_CTX3": "CAN1_TX",
    }
    if silicon_peripheral in canfd_to_e1m:
        return [canfd_to_e1m[silicon_peripheral]]

    return []


def _expected_functions(ref: str) -> list[str] | None:
    """E1M_<class><N> -> the e1m_function name(s) it must resolve to, or
    None when the class is unknown (a hard failure, not a silent pass)."""
    namespace = "e1m-x" if ref.startswith("E1M_X_") else "e1m"
    body = ref.removeprefix("E1M_X_") if namespace == "e1m-x" else ref.removeprefix("E1M_")
    m = re.fullmatch(r"(.+?)(\d*)", body)
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
    if cls == "ADC" and namespace == "e1m-x":
        return [f"ADC{n}"]
    if cls == "ADC":                      # E1M connector names analog pads ANA_S<N>
        return [f"ANA_S{n}"]
    if cls == "UART":
        return [f"UART{n}_RX", f"UART{n}_TX"]
    if cls == "I2C":
        if namespace == "e1m-x":
            return [f"I2C{n}.SCL", f"I2C{n}.SDA"]
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
            if isinstance(v, str) and v.startswith("E1M_"):
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
    funcs: set[str] = set()
    for p in doc.get("pads", []):
        funcs.add(p["e1m_function"])
        if family == "v2n":
            funcs.update(_v2n_function_aliases(p.get("silicon_peripheral", "")))
    return funcs


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
