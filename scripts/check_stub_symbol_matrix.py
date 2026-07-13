#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Compile/link symbol-matrix gate for the shared stub backend.

`src/common/stub_backend.c` is the link-complete fall-back: every public
`alp_*` entry point has a NOSUPPORT definition here, and a vendor backend
excludes a class's stubs by defining the matching `ALP_VENDOR_OVERRIDES_*`
compile macro (the umbrella `PERIPHERAL` implies `I2C/SPI/GPIO/UART`; the
independent `I2C_TARGET`, `SPI_TARGET`, `UART_RX_RINGBUF` sub-gates gate their
surfaces separately).  A handful of symbols (e.g. `alp_i2c_capabilities`) are
unguarded and must appear in *every* build.

Issue #673 splits this 1.7k-line monolith into one translation unit per API
class.  The failure mode that split introduces -- and that the existing pytest
suites do **not** exercise -- is a mis-set preprocessor gate that leaves a
symbol **defined twice** (two TUs) or **not at all** (no TU) under some
override combination, surfacing only as a downstream link error.

This gate is the characterization net for that split.  For each override
combination it compiles the stub source set (the monolith today; the per-class
TUs after the split -- discovered automatically) and, from the object files:

  * asserts every combination compiles;
  * asserts no public `alp_*` symbol is defined in more than one object
    (the duplicate-symbol guard the split needs);
  * asserts monotonicity -- enabling an override only ever *removes* symbols,
    never adds one (`defined(combo) subset-of defined(none)`), so a stray
    definition leaking into a gated build fails here;
  * asserts each single-class override actually removes at least one symbol
    (the gate has teeth); and
  * pins the exact public-symbol set of every combination to a committed
    golden (`tests/fixtures/stub-symbol-matrix/symbols.json`), so any drift in
    which class owns which symbol fails byte-for-byte.

The split is done only when this gate stays green with the monolith deleted.

Usage:

    python3 scripts/check_stub_symbol_matrix.py            # verify
    python3 scripts/check_stub_symbol_matrix.py --update   # rewrite golden
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GOLDEN = REPO / "tests" / "fixtures" / "stub-symbol-matrix" / "symbols.json"

# POSIX feature-test macros so `clock_nanosleep`/`CLOCK_MONOTONIC` in the delay
# primitive resolve regardless of the compiler's default standard mode.
CFLAGS = ["-D_DEFAULT_SOURCE", "-D_POSIX_C_SOURCE=200809L",
          "-I", str(REPO / "include"), "-I", str(REPO / "src"), "-c"]

# The full per-class gate list (kept in sync with stub_backend.c).  PERIPHERAL
# is the umbrella; the three *_TARGET / *_RX_RINGBUF gates are independent.
UMBRELLA = "ALP_VENDOR_OVERRIDES_PERIPHERAL"
UMBRELLA_IMPLIES = ["ALP_VENDOR_OVERRIDES_I2C", "ALP_VENDOR_OVERRIDES_SPI",
                    "ALP_VENDOR_OVERRIDES_GPIO", "ALP_VENDOR_OVERRIDES_UART"]
INDEPENDENT_SUBGATES = ["ALP_VENDOR_OVERRIDES_I2C_TARGET",
                        "ALP_VENDOR_OVERRIDES_SPI_TARGET",
                        "ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF"]
PER_CLASS = [
    "ALP_VENDOR_OVERRIDES_I2C", "ALP_VENDOR_OVERRIDES_SPI",
    "ALP_VENDOR_OVERRIDES_GPIO", "ALP_VENDOR_OVERRIDES_UART",
    "ALP_VENDOR_OVERRIDES_PWM", "ALP_VENDOR_OVERRIDES_ADC",
    "ALP_VENDOR_OVERRIDES_COUNTER", "ALP_VENDOR_OVERRIDES_I2S",
    "ALP_VENDOR_OVERRIDES_CAN", "ALP_VENDOR_OVERRIDES_RTC",
    "ALP_VENDOR_OVERRIDES_WDT", "ALP_VENDOR_OVERRIDES_DISPLAY",
    "ALP_VENDOR_OVERRIDES_CAMERA", "ALP_VENDOR_OVERRIDES_WIFI",
    "ALP_VENDOR_OVERRIDES_MQTT", "ALP_VENDOR_OVERRIDES_AUDIO_IN",
    "ALP_VENDOR_OVERRIDES_AUDIO_OUT", "ALP_VENDOR_OVERRIDES_BLE",
    "ALP_VENDOR_OVERRIDES_SECURITY", "ALP_VENDOR_OVERRIDES_INFERENCE",
    "ALP_VENDOR_OVERRIDES_POWER", "ALP_VENDOR_OVERRIDES_GPU2D",
    "ALP_VENDOR_OVERRIDES_RPC",
] + INDEPENDENT_SUBGATES

# Every combination is a (name, [macros]) pair.  Full 2**N is unnecessary --
# the gates are independent `#if !defined` blocks -- so we cover: no override,
# each gate alone, the umbrella, the umbrella plus every sub-gate, and all.
def _combos() -> list[tuple[str, list[str]]]:
    combos: list[tuple[str, list[str]]] = [("none", [])]
    for g in PER_CLASS:
        combos.append((g[len("ALP_VENDOR_OVERRIDES_"):].lower(), [g]))
    combos.append(("peripheral", [UMBRELLA]))
    combos.append(("peripheral+targets", [UMBRELLA] + INDEPENDENT_SUBGATES))
    combos.append(("all", [UMBRELLA] + PER_CLASS))
    return combos


def stub_sources() -> list[Path]:
    """Discover the stub source set -- split-ready.

    Today this is the single `stub_backend.c` monolith.  The #673 split moves
    the per-class stubs into `src/common/stub/*.c`; when that directory exists
    its units are picked up automatically, so this gate needs no edit to guard
    the split it exists to protect.
    """
    srcs: list[Path] = []
    mono = REPO / "src" / "common" / "stub_backend.c"
    if mono.is_file():
        srcs.append(mono)
    stub_dir = REPO / "src" / "common" / "stub"
    if stub_dir.is_dir():
        srcs.extend(sorted(stub_dir.glob("*.c")))
    if not srcs:
        raise SystemExit("check_stub_symbol_matrix: no stub sources found")
    return srcs


def _public_syms(obj: Path) -> set[str]:
    """Public `alp_*` symbols an object file *defines* (global, any section)."""
    rv = subprocess.run(["nm", "-g", "--defined-only", str(obj)],
                        capture_output=True, text=True, check=True)
    syms: set[str] = set()
    for line in rv.stdout.splitlines():
        parts = line.split()
        # `<addr> <type> <name>` or `<type> <name>` (undefined-addr form).
        if len(parts) >= 2 and parts[-2] in "TWDBRVC" and parts[-1].startswith("alp_"):
            syms.add(parts[-1])
    return syms


def _build_combo(cc: str, srcs: list[Path], macros: list[str],
                 workdir: Path) -> tuple[set[str], list[str]]:
    """Compile every source under `macros`; return (union symbols, duplicates)."""
    defs = [f"-D{m}=1" for m in macros]
    seen: dict[str, str] = {}
    dups: list[str] = []
    union: set[str] = set()
    for i, src in enumerate(srcs):
        obj = workdir / f"{i}_{src.stem}.o"
        rv = subprocess.run([cc, *CFLAGS, *defs, str(src), "-o", str(obj)],
                            capture_output=True, text=True, check=False)
        if rv.returncode != 0:
            raise SystemExit(
                f"check_stub_symbol_matrix: compile failed for {src.name} "
                f"with {' '.join(macros) or '(no overrides)'}:\n{rv.stderr}")
        for s in _public_syms(obj):
            if s in seen and seen[s] != src.name:
                dups.append(f"{s} (in {seen[s]} and {src.name})")
            seen.setdefault(s, src.name)
            union.add(s)
    return union, sorted(set(dups))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--update", action="store_true",
                    help="rewrite the golden symbol map instead of checking")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"),
                    help="C compiler to drive (default: $CC or cc)")
    args = ap.parse_args()

    cc = shutil.which(args.cc) or args.cc
    srcs = stub_sources()
    print(f"stub source set: {', '.join(s.name for s in srcs)}")

    result: dict[str, list[str]] = {}
    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="alp-stub-matrix-") as tmp:
        work = Path(tmp)
        for name, macros in _combos():
            union, dups = _build_combo(cc, srcs, macros, work)
            result[name] = sorted(union)
            if dups:
                errors.append(f"{name}: duplicate symbol(s): {'; '.join(dups)}")

    base = set(result["none"])
    # Structural invariants -- hold even if the golden is regenerated.
    for name, macros in _combos():
        syms = set(result[name])
        leaked = syms - base
        if leaked:  # monotonicity: an override must never *add* a symbol
            errors.append(f"{name}: symbol(s) absent from full stub: "
                          f"{', '.join(sorted(leaked))}")
        if len(macros) == 1 and not (base - syms):  # single-gate teeth
            errors.append(f"{name}: override removed no symbol (dead gate?)")

    GOLDEN.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.update:
        GOLDEN.write_text(text, encoding="utf-8")
        print(f"wrote {GOLDEN.relative_to(REPO)} "
              f"({len(result)} combos, {len(base)} full symbols)")
        if errors:  # still surface structural breakage on --update
            for e in errors:
                print(f"  ! {e}", file=sys.stderr)
            return 1
        return 0

    if not GOLDEN.is_file():
        print("check_stub_symbol_matrix: no golden (run --update)", file=sys.stderr)
        return 1
    if GOLDEN.read_text(encoding="utf-8") != text:
        errors.append("symbol map differs from golden "
                      "(run --update and review the diff)")

    if errors:
        print(f"\ncheck_stub_symbol_matrix: {len(errors)} problem(s):",
              file=sys.stderr)
        for e in errors:
            print(f"  · {e}", file=sys.stderr)
        return 1
    print(f"check_stub_symbol_matrix: {len(result)} override combination(s), "
          f"{len(base)} public symbols, no duplicates -- link-complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
