#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Plain-CMake link-completeness gate (issue #593).

Configures + builds the baremetal and yocto plain-CMake `libalp_sdk`
archives (ALP_OS=baremetal / ALP_OS=yocto, ALP_SOM=none, no optional
pkg-config-gated Yocto backends needed -- those degrade to sw_fallback,
which still exports every symbol), then compares the DEFINED symbols in
each archive against the full public function inventory recorded in
`metadata/catalog.json` (`portable_api[].functions`).

Fails (exit 1) when either library is missing a documented public
`alp_*` entry point -- the exact regression `src/common/stub_backend.c`'s
own doc comment promises never happens ("every documented entry point"
exported, even if every call returns `ALP_ERR_NOSUPPORT`).

Usage:
    python3 scripts/check_plain_cmake_link_complete.py
    python3 scripts/check_plain_cmake_link_complete.py --build-dir /tmp/x --keep
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CATALOG = REPO / "metadata" / "catalog.json"


def _portable_functions() -> set[str]:
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    funcs: set[str] = set()
    for entry in catalog["portable_api"]:
        funcs.update(entry["functions"])
    return funcs


def _configure_and_build(os_backend: str, build_dir: Path) -> Path:
    subprocess.run(
        ["cmake", "-B", str(build_dir), "-S", str(REPO), f"-DALP_OS={os_backend}"],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--parallel"],
        check=True,
        capture_output=True,
        text=True,
    )
    lib = build_dir / "libalp_sdk.a"
    if not lib.is_file():
        raise SystemExit(f"error: {lib} was not produced by the {os_backend} build")
    return lib


def _defined_symbols(archive: Path) -> set[str]:
    out = subprocess.run(
        ["nm", "--defined-only", str(archive)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    syms: set[str] = set()
    for line in out.splitlines():
        m = re.match(r"^[0-9a-fA-F]*\s+[A-Za-z]\s+(alp_\w+)$", line.strip())
        if m:
            syms.add(m.group(1))
    return syms


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="parent dir for the baremetal/ and yocto/ build trees "
        "(default: a fresh temp dir, removed on exit unless --keep)",
    )
    ap.add_argument(
        "--keep", action="store_true", help="don't delete a temp --build-dir on exit"
    )
    args = ap.parse_args()

    portable = _portable_functions()
    if not portable:
        print(f"error: no portable_api functions found in {CATALOG}", file=sys.stderr)
        return 2

    tmp_owned = args.build_dir is None
    build_root = args.build_dir or Path(tempfile.mkdtemp(prefix="alp-link-complete-"))
    build_root.mkdir(parents=True, exist_ok=True)

    errors: list[str] = []
    try:
        for os_backend in ("baremetal", "yocto"):
            build_dir = build_root / os_backend
            try:
                archive = _configure_and_build(os_backend, build_dir)
            except subprocess.CalledProcessError as exc:
                print(f"{os_backend} build FAILED:\n{exc.stdout}\n{exc.stderr}", file=sys.stderr)
                return 1
            defined = _defined_symbols(archive)
            missing = sorted(portable - defined)
            if missing:
                errors.append(
                    f"{os_backend}: {len(missing)}/{len(portable)} portable functions "
                    f"missing from {archive.name}:\n"
                    + "\n".join(f"    {name}" for name in missing)
                )
            else:
                print(f"{os_backend}: OK, all {len(portable)} portable functions present")
    finally:
        if tmp_owned and not args.keep:
            shutil.rmtree(build_root, ignore_errors=True)

    if errors:
        print("\nplain-cmake link-completeness check FAILED:", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
