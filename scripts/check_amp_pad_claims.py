#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Cross-core pad-claim gate for the AMP RZ/V2N SoM (issue #1142).

`metadata/pinmux/v2n.yaml` can now say WHICH core drives a pad
(`core: "a55"` / `"m33"`, issue #1157), but nothing read that field.
Meanwhile the A55's own pad claims live in a completely different tree --
the Linux devicetree under `meta-alp-sdk/recipes-kernel/linux/` -- and
neither side can see the other's claim at build time.  That blind spot is
not hypothetical: `docs/errata-e1m-x-v2n.md` records, as errata E3, the USB
over-current gpio-hog on `P9.6` silently clobbering the CM33's live
`SCK7` mux at every Linux boot after the GD32 supervisor SPI took the pad
on 2026-06-03, because the hog's `PMC9`/`PM9` byte-RMW lands at ~1.9 s,
inside the CM33's pin-setup window.  A bench session found that; nothing
in CI could.

This gate closes that loop in the only direction the metadata currently
supports: a Linux DT pad claim on a pad the metadata attributes to the
CM33 is an error.

WHAT COUNTS AS A LINUX PAD CLAIM.  Both port-pad macros the RZ/V2N
bindings expose, wherever they appear in a `.dts`/`.dtsi` under
`meta-alp-sdk/recipes-kernel/linux/`:

  * `RZV2N_GPIO(<port>, <pin>)`        -- gpio-hogs and gpio consumers
  * `RZV2N_PORT_PINMUX(<port>, <pin>, <func>)` -- pinctrl groups

Both resolve to pad `P<port><pin>` (`RZV2N_GPIO(A, 0)` -> `PA0`,
`RZV2N_PORT_PINMUX(0, 6, 1)` -> `P06`), which is the spelling
`metadata/pinmux/v2n.yaml` uses in `silicon_pad`.  Dedicated-ball groups
that name pins as strings (`pins = "SD0CLK"`) are not port pads and are
out of scope.

ponytail: textual claim, not node-status aware -- a claim inside a
`status = "disabled"` node, or in a pinctrl group nothing references, is
still reported.  That is deliberate and conservative: a CM33-owned pad
named in a Linux DT is worth a human look either way, and node-status
resolution needs a real dtc pass this gate is not worth spending.

Exit codes:
* 0  -- no Linux DT claim on a CM33-attributed pad.
* 1  -- one or more claims, or a stale exemption.

Run locally:

    python3 scripts/check_amp_pad_claims.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

PINMUX = Path("metadata") / "pinmux" / "v2n.yaml"
LINUX_DT_DIR = Path("meta-alp-sdk") / "recipes-kernel" / "linux"

# (peripheral, pad) pairs the metadata attributes to the CM33 that the
# Linux DT is nevertheless ALLOWED to claim.  Every entry needs both
# claims cited, and an entry matching no `core: "m33"` row is a hard
# error below -- an exemption that quietly stops applying is how a gate
# rots into decoration.
#
# BRD_I2C / RIIC8 is genuinely dual-master in this tree, and both masters
# are deliberate and shipped:
#   * CM33: zephyr/boards/alp/e1m_v2n101_m33_sm/
#     alp_e1m_v2n101_m33_sm-pinctrl.dtsi:37-42 muxes P06/P07, and
#     ..._r9a09g056n48gbg_cm33.dts:114-118 enables &i2c8 (the DA9292 PMIC
#     at 0x1E and the GD32 supervisor both answer there).
#   * A55: meta-alp-sdk/recipes-kernel/linux/linux-renesas/
#     e1m-x-evk.dtsi's `i2c8_pins` node muxes the same two pads and its
#     `&i2c8` node enables the controller; e1m-v2n-som.dtsi:256-264 puts
#     the `gd32_gpio` bridge expander on it, driven by the purpose-built
#     kernel driver in 0005-gpio-add-gd32-bridge-expander-driver.patch,
#     and e1m-x-evk.dtsi's panel `reset-gpios = <&gd32_gpio 5 ...>` makes
#     the Display-1 panel reset a real consumer.
#     (Nodes named rather than line-cited on purpose: this file shifts
#     whenever the carrier dtsi gains a node, and a stale pointer in the
#     one comment a maintainer reads to adjudicate a contested pad is
#     worse than no pointer.)
# `metadata/pinmux/v2n.yaml` says `core: "m33"` for both pads because the
# `core` field has no value for "both cores drive this" -- and its own
# documentation forbids reading an ABSENT `core` as "shared" too.  So the
# metadata cannot currently state the truth about these two pads either
# way.  Widening that vocabulary is a schema decision (#1157), not this
# gate's call; until it lands, the pair is exempt HERE, in one place, with
# the contradiction written down rather than silently tolerated.
EXEMPT: dict[tuple[str, str], str] = {
    ("RIIC8_SDA8", "P06"): "BRD_I2C is dual-master by design; see #1142/#1157",
    ("RIIC8_SCL8", "P07"): "BRD_I2C is dual-master by design; see #1142/#1157",
}

_GPIO_RE = re.compile(r"RZV2N_GPIO\(\s*([0-9A-Z])\s*,\s*(\d+)\s*\)")
_PINMUX_RE = re.compile(r"RZV2N_PORT_PINMUX\(\s*([0-9A-Z])\s*,\s*(\d+)\s*,\s*\d+\s*\)")


def _m33_pads(root: Path) -> dict[str, list[str]]:
    """pad -> peripherals, for every `core: "m33"` row in the pinmux table.

    Keyed pad -> LIST, because `(peripheral, pad)` is the table's real key:
    one pad can carry more than one row, and the two ends of an inter-chip
    link share a peripheral name across different pads.
    """
    doc = yaml.safe_load((root / PINMUX).read_text(encoding="utf-8"))
    out: dict[str, list[str]] = {}
    for pad in doc["pads"]:
        if pad.get("core") == "m33":
            out.setdefault(pad["silicon_pad"], []).append(
                pad["silicon_peripheral"])
    return out


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    pinmux = root / PINMUX
    if not pinmux.is_file():
        return [f"{PINMUX.as_posix()}: missing -- this gate cannot run without it"]
    m33 = _m33_pads(root)

    stale = [key for key in EXEMPT if key[0] not in m33.get(key[1], ())]
    for peripheral, pad in stale:
        problems.append(
            f"EXEMPT entry ({peripheral!r}, {pad!r}) matches no "
            f"`core: \"m33\"` row in {PINMUX.as_posix()} -- the attribution "
            f"it excuses is gone, so drop the entry from "
            f"scripts/check_amp_pad_claims.py"
        )

    dt_dir = root / LINUX_DT_DIR
    for path in sorted(dt_dir.rglob("*.dts*")):
        if path.suffix not in (".dts", ".dtsi"):
            continue
        for lineno, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            for match in (*_GPIO_RE.finditer(line), *_PINMUX_RE.finditer(line)):
                pad = f"P{match.group(1)}{match.group(2)}"
                for peripheral in m33.get(pad, ()):
                    if (peripheral, pad) in EXEMPT:
                        continue
                    rel = path.relative_to(root).as_posix()
                    problems.append(
                        f"{rel}:{lineno}: Linux devicetree claims {pad} "
                        f"({match.group(0)}), which {PINMUX.as_posix()} "
                        f"attributes to the CM33 (core: \"m33\", "
                        f"silicon_peripheral: {peripheral!r}).  A Linux port "
                        f"claim is a non-atomic PMC/PM byte-RMW at pinctrl "
                        f"probe (~1.9 s) that can clobber the CM33's live mux "
                        f"-- the errata E3 P9.6/SCK7 regression "
                        f"(errata E3, docs/errata-e1m-x-v2n.md).  Drop the claim, or "
                        f"correct the attribution in "
                        f"metadata/e1m_modules/v2n/core-ownership.yaml with "
                        f"evidence"
                    )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="repository root to check (default: this repo)")
    args = parser.parse_args()

    problems = find_problems(args.root)
    if problems:
        for p in problems:
            print(f"amp-pad-claims: {p}", file=sys.stderr)
        return 1
    print("OK: no Linux devicetree claim on a CM33-attributed RZ/V2N pad.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
