#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Pin every copy of the CM33 OpenAMP shared window to its one declaration.

The window (A55 base, CM33-NS base, size, rsctbl page) is declared once, in
the SoC description's `openamp_carveout` block
(metadata/socs/renesas/rzv2n/n44.json).  Four things restate it and must
agree with it, or the A55 and the CM33 map different memory and the link
dies with no error:

  - the generated CM33 board .dts (openamp_shm, rsctbl, mhu1_shm),
  - the Linux reserved-memory node and the rsctbl / mhu-shm generic-uio
    nodes (meta-alp-sdk .../e1m-v2n-som.dtsi, hand-written),
  - the RPC backend's generated header src/backends/rpc/alp_amp_window.h,
  - the beacon layout, which must be defined only in
    include/alp/protocol/amp_beacon.h (a second literal copy of the magic
    is how the three previous definitions drifted).

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
MAGIC_HOME = "include/alp/protocol/amp_beacon.h"
MAGIC_RE = re.compile(r"0xA10D0683", re.IGNORECASE)
SCAN_DIRS = ("src", "include", "examples", "firmware", "zephyr", "tests/yocto")


def _node_reg(text: str, header: str) -> list[int] | None:
    """The `reg` cells of the first node whose header line matches `header`."""
    m = re.search(header + r"\s*\{(?P<body>[^}]*)\}", text)
    r = m and re.search(r"reg\s*=\s*<([^>]*)>", m.group("body"))
    return [int(x, 16) for x in r.group(1).split()] if r else None


def find_problems(root: Path) -> list[str]:
    c = gen.load_carveout(root)
    a55, cm33, size, rsc = c["a55_base"], c["cm33_ns_base"], c["size"], c["rsctbl_size"]
    out: list[str] = []

    hdr = root / gen.OUT
    if not hdr.is_file() or hdr.read_text(encoding="utf-8").replace("\r\n", "\n") != gen.render(c):
        out.append(f"{gen.OUT}: stale vs openamp_carveout -- run python3 scripts/gen_amp_window.py")

    dtsi = root / LINUX_DTSI
    t = dtsi.read_text(encoding="utf-8") if dtsi.is_file() else ""
    for label, header, want in (
        ("reserved-memory", rf"openamp@{a55:x}", [0, a55, 0, size]),
        ("rsctbl uio", rf"rsctbl@{a55:x}", [0, a55, 0, rsc]),
        ("mhu-shm uio", rf"mhu-shm@{a55 + rsc:x}", [0, a55 + rsc, 0, 0x1000]),
    ):
        got = _node_reg(t, header)
        if got != want:
            out.append(
                f"{LINUX_DTSI}: {label} node {header} reg = {got}, openamp_carveout says {want}"
            )

    for rel in BOARD_DTS:
        p = root / rel
        t = p.read_text(encoding="utf-8") if p.is_file() else ""
        for label, header, want in (
            ("openamp_shm", rf"openamp_shm:\s*memory@{cm33:x}", [cm33, size]),
            ("rsctbl", rf"rsctbl:\s*memory@{cm33:x}", [cm33, rsc]),
            ("mhu1_shm", rf"mhu1_shm:\s*memory@{cm33 + rsc:x}", [cm33 + rsc, 0x1000]),
        ):
            got = _node_reg(t, header)
            if got != want:
                out.append(f"{rel}: {label} reg = {got}, openamp_carveout says {want}")

    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob("*")) if (root / d).is_dir() else []:
            rel = p.relative_to(root).as_posix()
            if p.suffix in (".c", ".h") and rel != MAGIC_HOME and MAGIC_RE.search(
                p.read_text(encoding="utf-8", errors="replace")
            ):
                out.append(f"{rel}: beacon magic restated -- use {MAGIC_HOME}")
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
