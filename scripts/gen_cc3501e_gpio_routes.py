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

A composed row whose target CC3501E pad is bridge-reserved (a row whose
`from-cc3501e.tsv` peripheral column names a bridge claim -- BRIDGE_READY,
BRIDGE_SPI_CSN -- rather than a bare GPIOxx pad) is excluded here, at
GENERATION time -- i.e. a build-time failure mode for the SDK maintainer
regenerating this file, not a runtime ALP_ERR_INVAL from
cc3501e_gpio_configure() on a device.  See RESERVED_CC3501E_PADS below;
each excluded row is printed, not silently dropped.

Run:

    python3 scripts/gen_cc3501e_gpio_routes.py

CI (pr-generated-files.yml) regenerates every touched example's route
table on any PR that touches its board.yaml/prj.conf or the AEN SoM /
hw-revisions metadata, then fails if the working tree diff is non-empty.
"""

from __future__ import annotations

import csv
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
EXAMPLES_DIR = REPO / "examples" / "aen"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"
FROM_CC3501E_TSV = REPO / "metadata" / "e1m_modules" / "aen" / "from-cc3501e.tsv"

_TSV_IO_RE = re.compile(r"IO\d+")
_PLAIN_GPIO_RE = re.compile(r"GPIO_?(\d+)")


def _reserved_pads_from_tsv() -> frozenset[int]:
    """CC3501E GPIO indices reserved from the host GPIO proxy, derived from
    FROM_CC3501E_TSV rather than hand-typed against
    cc3501e-bridge-firmware's hal/ti/cc3501e_hw_ti_gpio.c
    gpio_pad_reserved() -- that firmware lives in a SEPARATE repo
    (alplabai/cc3501e-bridge-firmware; moved out of alp-sdk by #1370/#1805),
    so a literal copied from it here could drift with no way to detect it.

    A TSV row whose `e1m_function` is an E1M IOxx pad and whose
    `cc3501e_function` is a NAMED claim (`BRIDGE_READY`, `BRIDGE_SPI_CSN`
    -- #1808's peripheral-column convention) rather than a bare `GPIOxx`
    pad name marks that pad's raw index (in `cc3501e_pad`) reserved.  This
    only captures pads reachable via an E1M IOxx pad_routes entry in the
    first place (the bridge's own inter-chip SPI0 pads never are, since
    they surface in the TSV as `SPI1_*` rows, not `IOxx` ones) --
    test_aen_som_gpio_pad_routes_match_tsv_source already enforces that
    every SoM's `pad_routes:` IOxx entry matches this TSV, so a pad_routes
    entry can never target an unlisted pad without that gate catching it
    first.
    """
    reserved: set[int] = set()
    with FROM_CC3501E_TSV.open(newline="", encoding="utf-8") as f:
        rows = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            if not _TSV_IO_RE.fullmatch(row.get("e1m_function") or ""):
                continue
            func = row.get("cc3501e_function") or ""
            if _PLAIN_GPIO_RE.fullmatch(func):
                continue  # unclaimed -- an ordinary proxyable pad
            m = _PLAIN_GPIO_RE.fullmatch(row.get("cc3501e_pad") or "")
            if m:
                reserved.add(int(m.group(1)))
    return frozenset(reserved)


RESERVED_CC3501E_PADS = _reserved_pads_from_tsv()

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
            # Unlike E1M_SPI1 (excluded above by the IOxx regex, and
            # legitimately pin-less), a GPIO row that reaches here with
            # dispatch: cc3501e and no dispatch_pin is always a metadata
            # defect (typo'd dispatch, a pad_route_overrides entry missing
            # its pin, ...) -- fail loudly instead of silently delegating
            # the pad to the platform driver (#1859 review).
            sys.exit(
                f"gen_cc3501e_gpio_routes: {e1m} has dispatch: cc3501e but no "
                f"dispatch_pin (hw_rev={composed.get('hw_rev')}) -- fix its "
                f"pad_routes / pad_route_overrides entry"
            )
        pin = int(pin)
        if pin in RESERVED_CC3501E_PADS:
            # Not silent: #1859's whole point is that a reserved-pad route
            # must be visible, not discovered at cc3501e_gpio_configure()
            # runtime.  Always dropped (the firmware refuses it), never a
            # build failure -- IO16/IO17 are a permanent physical fact of
            # this SoM family, not a transient metadata bug.
            print(f"  dropping {e1m} -> CC3501E GPIO_{pin} (bridge-reserved pad)")
            continue
        doc = (row.get("board_doc") or row.get("som_doc") or "").replace(
            "*/", "* /").replace("\n", " ")
        rows.append((e1m, pin, doc))

    # Defensive, not compiled out under -O (unlike `assert`): the filter
    # above is the single place that decides "never proxy a reserved pad".
    # Re-check it actually held, so a future edit that breaks the filter
    # fails HERE (generation time) instead of at cc3501e_gpio_configure()
    # on a device (#1859).
    if any(pin in RESERVED_CC3501E_PADS for _, pin, _ in rows):
        sys.exit("gen_cc3501e_gpio_routes: internal error -- a reserved pad "
                  "survived the filter")
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
    # file:<path>, not bare "file": bare "file" makes clang-format search
    # upward from `path`'s OWN directory for a .clang-format, which finds
    # nothing (falling back to the LLVM default style) when `path` is
    # redirected outside the repo tree -- e.g. by a test's _out_path_for()
    # monkeypatch into a tmp dir.  Pinning the repo's own file makes
    # formatting identical regardless of where the caller points output.
    subprocess.run(
        [exe, "-i", f"--style=file:{REPO / '.clang-format'}", str(path)], check=True
    )


def _out_path_for(app_dir: Path) -> Path:
    """Where <app_dir>'s route table is written.  A single indirection
    point (not a per-app inline expression in main()) so a test can
    monkeypatch it to write into a tmp dir instead of the tracked
    examples/ tree -- gen_board_header.py's OUT_DIR does the same job for
    its single shared output root; this generator has one output root per
    app instead, hence a function rather than a constant."""
    return app_dir / "src" / "cc3501e_gpio_routes.c"


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
        out_path = _out_path_for(app_dir)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        # newline="": force LF regardless of host OS (.gitattributes pins
        # *.c to LF; Path.write_text()'s default text-mode translation
        # would otherwise emit CRLF on Windows, matching gen_soc_caps.py's
        # OUT.write_text(..., newline="") for the same reason).
        out_path.write_text(out_text, encoding="utf-8", newline="")
        _clang_format(out_path, exe)
        print(f"wrote {out_path} ({len(rows)} routes, hw_rev={composed['hw_rev']})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
