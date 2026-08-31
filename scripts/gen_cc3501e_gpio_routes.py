#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate <example>/src/cc3501e_gpio_routes.c -- the CC3501E GPIO proxy's
board route table -- from the composed (SoM x board x hw_rev) pad-route
table, for every AEN example that enables the proxy backend.

Previously this file was hand-triplicated across aen-cc3501e-gpio,
aen-cc3501e-bringup and aen-cc3501e-companion-tour, hardcoding the r2
(production) map and never consulting `som.hw_rev` -- so a board.yaml
built against the r1 rev (IO8/IO10 are direct Alif GPIOs on r1; IO21,
not IO8/IO10, reaches the CC3501E there) would proxy the WRONG pins
(issue #1859).  It also hand-omitted only IO17 as a bridge-reserved pad
and missed that IO16 (-> CC3501E GPIO_17, the bridge's own READY/host-IRQ
line) is reserved too, which the firmware's gpio_pad_reserved() rejects
at runtime.

This generator instead shells out to `alp_project.py --input <board.yaml>
--emit composed-route-table`, which already resolves per-revision routing
(metadata/e1m_modules/E1M-AEN801.yaml `pad_routes:` + the selected
hw_rev's overrides in metadata/e1m_modules/aen/hw-revisions.yaml, see
scripts/alp_project_loader.py `_hwrev_pad_route_overrides`), and derives
the table from that JSON -- so a board.yaml that declares `som.hw_rev:
r1` gets the r1 map automatically, and the three examples can never drift
against each other or against metadata again.

A composed row whose target CC3501E pad is bridge-reserved (the
transport's own SPI0 + console pads, or the bridge's own READY/CS lines)
is excluded here, at GENERATION time -- i.e. a build-time failure mode
for the SDK maintainer regenerating this file, not a runtime
ALP_ERR_INVAL from cc3501e_gpio_configure() on a device.  See
RESERVED_CC3501E_PADS below.

Run:

    python3 scripts/gen_cc3501e_gpio_routes.py

CI (pr-generated-files.yml) regenerates every touched example's route
table on any PR that touches its board.yaml/prj.conf or the AEN SoM /
hw-revisions metadata, then fails if the working tree diff is non-empty.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXAMPLES_DIR = REPO / "examples" / "aen"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"

# Mirrors firmware/cc3501e/hal/ti/cc3501e_hw_ti_gpio.c gpio_pad_reserved():
# the bridge's own inter-chip SPI0 (CSN=16, SCLK=27, POCI=28, PICO=29), its
# UART2 console glue (TX=5, RX=6), the pads not bonded on this device
# (7/8/9), and GPIO_17 -- the bridge READY/host-IRQ line (E1M IO16).  A
# pad_routes / hw-revisions entry that targets one of these can never
# actually be proxied: the firmware refuses it at runtime.  The generator
# drops it here instead, as ONE rule applied to every row, rather than the
# hand-picked per-instance omission that caused #1859 (IO17 -- GPIO_16 --
# was correctly left out by hand, but IO16 -- GPIO_17 -- was not).
RESERVED_CC3501E_PADS = frozenset({5, 6, 7, 8, 9, 16, 17, 27, 28, 29})

_E1M_GPIO_RE = re.compile(r"E1M_GPIO_IO\d+")

# Declarative "this example uses the GPIO proxy" signal: the Kconfig the
# proxy backend is gated on (zephyr/CMakeLists.txt
# zephyr_library_sources_ifdef(CONFIG_ALP_SDK_GPIO_CC3501E_PROXY ...)).  An
# example that opens plain Alif GPIOs on the same SoM (e.g.
# aen-cc3501e-gatt-register) doesn't set this and gets no generated table.
_PROXY_KCONFIG = "CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y"


def _discover_targets() -> list[Path]:
    """Every examples/aen/<app>/board.yaml whose sibling prj.conf enables
    the CC3501E GPIO proxy backend."""
    targets = []
    for prj_conf in sorted(EXAMPLES_DIR.glob("*/prj.conf")):
        if _PROXY_KCONFIG not in prj_conf.read_text(encoding="utf-8").splitlines():
            continue
        board_yaml = prj_conf.parent / "board.yaml"
        if board_yaml.is_file():
            targets.append(board_yaml)
    return targets


def _composed_routes(board_yaml: Path) -> dict:
    proc = subprocess.run(
        [sys.executable, str(ALP_PROJECT), "--input", str(board_yaml),
         "--emit", "composed-route-table"],
        capture_output=True, text=True, check=False,
    )
    if proc.returncode != 0:
        sys.exit(
            f"gen_cc3501e_gpio_routes: alp_project.py failed for "
            f"{board_yaml.relative_to(REPO)}:\n{proc.stderr}"
        )
    return json.loads(proc.stdout)


def _proxy_rows(composed: dict) -> list[tuple[str, int, str]]:
    """Filter composed routes down to CC3501E-dispatched GPIO pads, in
    E1M-pad order, excluding bridge-reserved targets."""
    rows: list[tuple[str, int, str]] = []
    for row in composed["routes"]:
        if row.get("dispatch") != "cc3501e":
            continue
        e1m = row.get("e1m", "")
        if not _E1M_GPIO_RE.fullmatch(e1m):
            continue
        pin = row.get("dispatch_pin")
        if pin is None:
            continue
        pin = int(pin)
        if pin in RESERVED_CC3501E_PADS:
            continue
        doc = row.get("board_doc") or row.get("som_doc") or ""
        rows.append((e1m, pin, doc))

    # Defensive: the filter above is the single place that decides "never
    # proxy a reserved pad".  Assert it actually held, so a future edit to
    # this function that breaks the filter fails HERE (generation time)
    # instead of at cc3501e_gpio_configure() on a device (#1859).
    assert all(pin not in RESERVED_CC3501E_PADS for _, pin, _ in rows)
    return rows


def _emit(app_name: str, hw_rev: str, rows: list[tuple[str, int, str]]) -> str:
    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " * Copyright 2026 Alp Lab AB",
        " *",
        f" * Auto-generated for {app_name} by scripts/gen_cc3501e_gpio_routes.py",
        " * from metadata/e1m_modules/E1M-AEN801.yaml `pad_routes:` (resolved for",
        f" * hw_rev={hw_rev} -- see metadata/e1m_modules/aen/hw-revisions.yaml",
        " * `pad_route_overrides:` when board.yaml sets `som.hw_rev:`).",
        " * DO NOT EDIT BY HAND -- regenerate:",
        " *   python3 scripts/gen_cc3501e_gpio_routes.py",
        " *",
        " * Strong override of the WEAK cc3501e_gpio_routes[] /",
        " * cc3501e_gpio_route_count in",
        " * src/backends/gpio/cc3501e_proxy_routes_weak.c.  Maps the portable E1M",
        " * GPIO pin_id (alp_gpio_open(ALP_E1M_GPIO_IOxx)) to the RAW CC3501E GPIO",
        " * index the inter-chip bridge drives, so an alp_gpio_* call on a proxied",
        " * E1M IO is routed over the bridge while the Alif's own pins delegate to",
        " * the platform driver.",
        " *",
        " * Pads whose CC3501E target is bridge-reserved (the transport's own",
        " * SPI0 / console pads, or the bridge's own READY/host-IRQ + SPI0-CS",
        " * lines) are never emitted here -- the firmware's gpio_pad_reserved()",
        " * would refuse them at runtime, so the generator excludes them at",
        " * generation time instead (issue #1859).",
        " */",
        "",
        "#include <stddef.h>",
        "",
        "#include <alp/chips/cc3501e.h>",
        "#include <alp/e1m_pinout.h>",
        "",
        "const cc3501e_gpio_route_t cc3501e_gpio_routes[] = {",
    ]
    for e1m, pin, doc in rows:
        entry = f"\t{{ ALP_{e1m}, {pin}u }},"
        lines.append(f"{entry} /* {doc} */" if doc else entry)
    lines += [
        "};",
        "",
        "const size_t cc3501e_gpio_route_count =",
        "    sizeof(cc3501e_gpio_routes) / sizeof(cc3501e_gpio_routes[0]);",
        "",
    ]
    return "\n".join(lines)


def _clang_format_exe() -> str:
    """Resolve the pinned clang-format binary, or fail naming what's missing.

    Pinned to clang-format-22 (the CI version).  Exits 99 (not the default
    1) rather than degrading to a warning-and-leave-unformatted: a
    generator that writes raw, pre-aligned output when clang-format is
    absent gets diffed against the clang-formatted file already
    committed, misreporting a missing tool as generator "drift" instead
    of the SKIP it actually is -- the same trap gen_soc_caps.py's own
    _clang_format_exe() was hardened against (alp-sdk#1109/#1221);
    test-all.sh's stage_generated_files maps rc=99 to SKIP.
    """
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if exe is None:
        print(
            "error: clang-format not found on PATH; cannot format the "
            "generated CC3501E route tables to match the repo .clang-format "
            "(install clang-format==22.* -- see docs/testing.md)",
            file=sys.stderr,
        )
        raise SystemExit(99)
    return exe


def _clang_format(path: Path, exe: str) -> None:
    subprocess.run([exe, "-i", "--style=file", str(path)], check=True)


def main() -> int:
    targets = _discover_targets()
    if not targets:
        print("gen_cc3501e_gpio_routes: no example enables "
              f"{_PROXY_KCONFIG}", file=sys.stderr)
        return 1

    exe = _clang_format_exe()

    for board_yaml in targets:
        app_dir = board_yaml.parent
        composed = _composed_routes(board_yaml)
        rows = _proxy_rows(composed)
        out_text = _emit(app_dir.name, composed["hw_rev"], rows)
        out_path = app_dir / "src" / "cc3501e_gpio_routes.c"
        # newline="": force LF regardless of host OS (.gitattributes pins
        # *.c to LF; Path.write_text()'s default text-mode translation
        # would otherwise emit CRLF on Windows, matching gen_soc_caps.py's
        # OUT.write_text(..., newline="") for the same reason).
        out_path.write_text(out_text, encoding="utf-8", newline="")
        _clang_format(out_path, exe)
        print(f"wrote {out_path.relative_to(REPO)} ({len(rows)} routes, "
              f"hw_rev={composed['hw_rev']})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
