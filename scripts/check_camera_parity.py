#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Camera completeness gate (#2633): a camera module that cannot be generated, or
whose facts contradict its chip or carrier, cannot merge.

Adding a camera is a metadata-only change (scripts/gen_camera_dt.py derives the
Linux devicetree fragment and the kernel config from it), so this gate is what
makes "metadata only" safe.  For every metadata/camera_modules/*.yaml it fails,
listing exactly what is missing:

  * the chip exists; module lanes are in the chip's mipi.lanes_supported and fit
    at least one carrier camera connector; the I2C address is one of the chip's
    i2c.addresses; the xclk is in drivers.linux.xclk_supported_hz when declared;
  * when a Linux SoM hosts the chip (its `families` + a SoC with linux_dt):
    drivers.linux.compatible and .kconfig exist, link_freqs has an entry for the
    module's lane count when the chip declares any, and the compatible/kconfig
    is listed in metadata/os/linux-kernel-drivers.yaml with every named patch
    present under meta-alp-sdk/;
  * the committed camera-sensors.cfg contains every module's kconfig as =y;
  * every `ALP_CAMERA_<connector> = "<module>"` value in docs/ names a module
    that has a generated fragment for that connector.

Usage: python3 scripts/check_camera_parity.py [--root ROOT]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import gen_camera_dt as gen  # noqa: E402

_KNOB = re.compile(r'ALP_CAMERA_(CAM\d+)\s*=\s*"([^"<>]*)"')


def _kernel_entries(root: Path) -> list[dict]:
    p = root / "metadata" / "os" / "linux-kernel-drivers.yaml"
    if not p.is_file():
        return []
    doc = gen._yaml(p) or {}
    return [d for k in (doc.get("kernels") or {}).values() for d in k.get("drivers") or []]


def find_problems(root: Path) -> list[str]:
    tree = gen.Tree(root)
    out: list[str] = []
    kernel = _kernel_entries(root)
    cfg_path = root / gen.LINUX_DIR / gen.CFG_NAME
    cfg = cfg_path.read_text(encoding="utf-8") if cfg_path.is_file() else ""
    if not cfg:
        out.append(f"{gen.LINUX_DIR / gen.CFG_NAME}: missing; run python3 scripts/gen_camera_dt.py")
    cfg_syms = {ln.split("=")[0] for ln in cfg.splitlines() if ln.endswith("=y")}
    max_lanes = max((c["lanes"] for b in tree.boards.values()
                     for c in b["camera_connectors"].values()), default=0)

    for mid, mod in tree.modules.items():
        who = f"camera module {mid}"
        chip = tree.chip(mod["chip"])
        if chip is None:
            out.append(f"{who}: chip {mod['chip']} has no metadata/chips/{mod['chip']}.yaml")
            continue
        lanes_ok = (chip.get("mipi") or {}).get("lanes_supported") or []
        if mod["lanes"] not in lanes_ok:
            out.append(f"{who}: lanes {mod['lanes']} not in chip {mod['chip']} "
                       f"mipi.lanes_supported {lanes_ok}")
        addrs = [a["addr_7bit"] for a in (chip.get("i2c") or {}).get("addresses") or []]
        if mod["i2c_addr_7bit"] not in addrs:
            out.append(f"{who}: i2c_addr_7bit {gen._hex(mod['i2c_addr_7bit'])} not in chip "
                       f"{mod['chip']} i2c.addresses {[gen._hex(a) for a in addrs]}")
        lin = (chip.get("drivers") or {}).get("linux") or {}
        xclks = lin.get("xclk_supported_hz")
        if xclks and mod["xclk_hz"] not in xclks:
            out.append(f"{who}: xclk_hz {mod['xclk_hz']} not in chip {mod['chip']} "
                       f"drivers.linux.xclk_supported_hz {xclks}")
        if lin.get("kconfig") and lin["kconfig"] not in cfg_syms:
            out.append(f"{who}: {lin['kconfig']} is not =y in {gen.LINUX_DIR / gen.CFG_NAME}; "
                       "run python3 scripts/gen_camera_dt.py")

        hosts = tree.hosts(chip)
        if not hosts:
            continue  # no Linux SoM carries this chip
        where = f"hosted by {', '.join(sorted(s['sku'] for s in hosts))}"
        if mod["lanes"] > max_lanes:
            out.append(f"{who}: {mod['lanes']} lanes fit no carrier camera connector "
                       f"(widest has {max_lanes}); no fragment can be generated")
        for key in ("compatible", "kconfig"):
            if not lin.get(key):
                out.append(f"{who}: chip {mod['chip']} drivers.linux.{key} is missing ({where})")
        freqs = lin.get("link_freqs")
        if freqs and mod["lanes"] not in [e["lanes"] for e in freqs]:
            out.append(f"{who}: chip {mod['chip']} drivers.linux.link_freqs has no entry for "
                       f"{mod['lanes']} lanes ({where})")
        if lin.get("compatible"):
            hit = [d for d in kernel if d.get("compatible") == lin["compatible"]
                   and d.get("kconfig") == lin.get("kconfig")]
            if not hit:
                out.append(f"{who}: no BSP kernel driver for {lin['compatible']} / "
                           f"{lin.get('kconfig')} in metadata/os/linux-kernel-drivers.yaml ({where})")
            for d in hit:
                for patch in d.get("patches") or []:
                    if not (root / gen.LINUX_DIR / patch).is_file():
                        out.append(f"{who}: kernel patch {patch} named in "
                                   f"linux-kernel-drivers.yaml is not under {gen.LINUX_DIR}")

    # docs knob values must be generated fragments.
    generated = {(p["connector"], p["module"]) for p in gen._pairs(tree)[0]}
    for md in sorted((root / "docs").rglob("*.md")):
        for conn, val in _KNOB.findall(md.read_text(encoding="utf-8")):
            if val and (conn, val) not in generated:
                valid = sorted(m for c, m in generated if c == conn)
                out.append(f"{md.relative_to(root).as_posix()}: ALP_CAMERA_{conn} = \"{val}\" has no "
                           f"generated fragment (valid: {', '.join(valid) or 'none'})")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", type=Path, default=REPO)
    problems = find_problems(ap.parse_args(argv).root)
    for p in problems:
        print(f"check_camera_parity: {p}", file=sys.stderr)
    if problems:
        print("check_camera_parity: fix the metadata above (see docs/v2n-camera-csi.md, "
              "'Adding a camera'), then run python3 scripts/gen_camera_dt.py.", file=sys.stderr)
        return 1
    print("check_camera_parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
