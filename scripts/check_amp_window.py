#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Pin every copy of the CM33 OpenAMP shared window to its one declaration.

The window and its sub-regions (rsctbl page, mhu-shm, vring-ctl0/1,
vring-shm0/1) are declared once, as offsets+sizes inside the SoC
description's `openamp_carveout` block
(metadata/socs/renesas/rzv2n/n44.json).  Things that restate it must agree
with it, or the A55 and the CM33 map different memory and the link dies
with no error:

  - the generated CM33 board .dts (openamp_shm and one node per region),
  - the Linux reserved-memory node and the generic-uio node of every region
    (meta-alp-sdk .../e1m-v2n-som.dtsi, hand-written); each must also lie
    inside the carveout,
  - the RPC backend's generated header src/backends/rpc/alp_amp_window.h,
  - the beacon, defined only in include/alp/protocol/amp_beacon.h: C sources
    and headers under src/, include/, examples/, firmware/, zephyr/ and
    tests/yocto/ may not restate the magic or a beacon word address (a second
    literal copy is how the three previous definitions drifted).  YAML HIL
    specs under tests/hil/ are shell scripts and may quote the beacon, but
    every `devmem` address must be a current beacon word and a magic literal
    must equal the header's.  Markdown is not scanned.

    python3 scripts/check_amp_window.py [--root ROOT]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_amp_window as gen  # noqa: E402

LINUX_DTSI = "meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi"
BOARD_DTS = (
    "zephyr/boards/alp/e1m_v2n101_m33_sm/alp_e1m_v2n101_m33_sm_r9a09g056n48gbg_cm33.dts",
    "zephyr/boards/alp/e1m_v2m101_m33_sm/alp_e1m_v2m101_m33_sm_r9a09g056n48gbg_cm33.dts",
)
# region name (metadata) -> label of its node in the CM33 board .dts
BOARD_LABEL = {
    "rsctbl": "rsctbl",
    "mhu-shm": "mhu1_shm",
    "vring-ctl0": "vring_ctrl0",
    "vring-ctl1": "vring_ctrl1",
    "vring-shm0": "vring_shm0",
    "vring-shm1": "vring_shm1",
}
MAGIC_HOME = "include/alp/protocol/amp_beacon.h"
SCAN_DIRS = ("src", "include", "examples", "firmware", "zephyr", "tests/yocto", "tests/hil")
SCAN_SUFFIXES = (".c", ".h", ".yaml", ".yml")
BEACON_BYTES = 0x10  # include/alp/protocol/amp_beacon.h ALP_AMP_BEACON_SIZE


def _node_reg(text: str, header: str) -> list[int] | None:
    """The `reg` cells of the first node whose header line matches `header`."""
    m = re.search(header + r"\s*\{(?P<body>[^}]*)\}", text)
    r = m and re.search(r"reg\s*=\s*<([^>]*)>", m.group("body"))
    return [int(x, 16) for x in r.group(1).split()] if r else None


def _geometry_problems(c: dict) -> list[str]:
    """The metadata itself: every region lies inside the carveout, none overlap."""
    out, spans = [], []
    for name, r in c["regions"].items():
        if r["offset"] + r["size"] > c["size"]:
            out.append(f"openamp_carveout.regions.{name} runs past the carveout size {c['size']:#x}")
        spans.append((r["offset"], r["offset"] + r["size"], name))
    spans.sort()
    out += [
        f"openamp_carveout.regions {a[2]} and {b[2]} overlap"
        for a, b in zip(spans, spans[1:])
        if b[0] < a[1]
    ]
    return out


def find_problems(root: Path) -> list[str]:
    c = gen.load_carveout(root)
    a55, cm33, size = c["a55_base"], c["cm33_ns_base"], c["size"]
    regs = gen.regions(c)
    out: list[str] = _geometry_problems(c)

    hdr = root / gen.OUT
    if not hdr.is_file() or hdr.read_text(encoding="utf-8").replace("\r\n", "\n") != gen.render(c):
        out.append(f"{gen.OUT}: stale vs openamp_carveout -- run python3 scripts/gen_amp_window.py")

    dtsi = root / LINUX_DTSI
    t = dtsi.read_text(encoding="utf-8") if dtsi.is_file() else ""
    nodes = [("reserved-memory", rf"openamp@{a55:x}", [0, a55, 0, size])]
    nodes += [(f"{n} uio", rf"{re.escape(n)}@{pa:x}", [0, pa, 0, sz]) for n, (pa, sz) in regs.items()]
    for label, header, want in nodes:
        got = _node_reg(t, header)
        if got != want:
            out.append(f"{LINUX_DTSI}: {label} node {header} reg = {got}, openamp_carveout says {want}")
        elif not (a55 <= got[1] and got[1] + got[3] <= a55 + size):
            out.append(f"{LINUX_DTSI}: {label} node lies outside [{a55:#x}, {a55 + size:#x})")

    board = [("openamp_shm", rf"openamp_shm:\s*memory@{cm33:x}", [cm33, size])]
    for n, (pa, sz) in regs.items():
        da = cm33 + (pa - a55)
        board.append((BOARD_LABEL[n], rf"{BOARD_LABEL[n]}:\s*memory@{da:x}", [da, sz]))
    for rel in BOARD_DTS:
        p = root / rel
        t = p.read_text(encoding="utf-8") if p.is_file() else ""
        for label, header, want in board:
            got = _node_reg(t, header)
            if got != want:
                out.append(f"{rel}: {label} reg = {got}, openamp_carveout says {want}")
            elif not (cm33 <= got[0] and got[0] + got[1] <= cm33 + size):
                out.append(f"{rel}: {label} lies outside [{cm33:#x}, {cm33 + size:#x})")

    home = root / MAGIC_HOME
    mm = re.search(r"ALP_AMP_BEACON_MAGIC\s+(0x[0-9A-Fa-f]+)u", home.read_text(encoding="utf-8")) if home.is_file() else None
    if not mm:
        return out + [f"{MAGIC_HOME}: ALP_AMP_BEACON_MAGIC not found"]
    magic = mm.group(1).lower()
    # A55 address of each beacon word (the last BEACON_BYTES of the rsctbl page) and its CM33-NS alias.
    end = regs["rsctbl"][0] + regs["rsctbl"][1]
    words = [end - BEACON_BYTES + 4 * i for i in range(BEACON_BYTES // 4)]
    c_literals = re.compile(
        "|".join(re.escape(x) for x in [magic] + [f"{w:x}" for w in words] + [f"{w - a55 + cm33:x}" for w in words]),
        re.IGNORECASE,
    )
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob("*")) if (root / d).is_dir() else []:
            rel = p.relative_to(root).as_posix()
            if p.suffix not in SCAN_SUFFIXES or rel == MAGIC_HOME:
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            if p.suffix in (".c", ".h"):
                # C cannot be excused: it includes the header and the generated window.
                if m := c_literals.search(text):
                    out.append(f"{rel}: beacon magic/address restated ({m.group(0)}) -- use {MAGIC_HOME}")
                continue
            # A HIL spec is a shell script and cannot include a header, so it may quote the
            # beacon -- but only correctly: each devmem address must be a current beacon word
            # and any magic-looking (0xa10dXXXX) literal must be the current magic.
            for m in re.finditer(r"devmem\s+(0x[0-9a-fA-F]+)", text):
                if int(m.group(1), 16) not in words:
                    out.append(f"{rel}: devmem {m.group(1)} is not a beacon word of the current rsctbl page")
            for m in re.finditer(r"0xa10d[0-9a-f]{4}", text, re.IGNORECASE):
                if m.group(0).lower() != magic:
                    out.append(f"{rel}: magic {m.group(0)} != {magic} in {MAGIC_HOME}")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--root", type=Path, default=gen.REPO)
    problems = find_problems(ap.parse_args().root)
    for p in problems:
        print(p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
