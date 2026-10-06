#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Gate: the committed V2N/V2M Linux camera files match the camera metadata (#2633).

Every camera module in metadata/camera_modules/ must have a generated
devicetree fragment for each carrier camera connector it fits
(e1m-x-evk-<connector>-<module_id>.dtsi) and its sensor driver in
camera-sensors.cfg, and no generated fragment may outlive its module.  The
check regenerates everything in memory with scripts/gen_camera_dt.py and
compares bytes, so a hand edit of a generated file, a metadata change with no
regeneration and a module with no driver info all fail here.  Fix: run
`python3 scripts/gen_camera_dt.py` and commit the result.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import gen_camera_dt as g  # noqa: E402


def find_problems(root: Path) -> list[str]:
    try:
        want = g.generate(root)
    except (g.GenError, KeyError, OSError) as e:
        return [f"gen_camera_dt cannot generate: {type(e).__name__}: {e}"]
    return g.stale_files(root, want)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    problems = find_problems(ap.parse_args().root)
    for p in problems:
        print(f"check_camera_parity: {p} -- run python3 scripts/gen_camera_dt.py", file=sys.stderr)
    if not problems:
        print("OK   camera DT + kernel config match metadata")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
