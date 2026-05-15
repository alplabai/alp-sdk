# SPDX-License-Identifier: Apache-2.0
"""
`west alp-flash` -- walk system-manifest.yaml and program every
slice onto attached hardware in the order dictated by `boot_order:`.

Phase 2 ships the dispatch + stub backends; the real per-helper-MCU
flash backends (openocd for GD32 via SWD, the CC3501E USB-CDC
bootloader, vendor SWD tools for the A-cluster + M-class peers)
land in Phase 3+.

The backend dispatch table lives in this file (rather than the SoM
preset) so Phase 3 can iterate without touching the metadata
contract.  Customer flow:

    west alp-build examples/rpmsg-v2n
    west alp-image examples/rpmsg-v2n      # optional: pre-build bundle
    west alp-flash examples/rpmsg-v2n      # respects boot_order
    west alp-flash examples/rpmsg-v2n --dry-run   # just print
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path
from typing import Optional

import yaml                                  # type: ignore[import-untyped]
from west import log                         # type: ignore[import-not-found]
from west.commands import WestCommand        # type: ignore[import-not-found]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root        # noqa: E402


# Backend-name -> required executable.  Phase 2 just prints a stub
# command; Phase 3 replaces these with the real invocations.
_BACKEND_TOOLS: dict[str, str] = {
    "openocd":       "openocd",
    "bitbake-flash": "bitbake",
    "vendor-swd":    "JLinkExe",
    "stub":          "echo",
}


class AlpFlash(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-flash",
            "Walk system-manifest.yaml and program every slice",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):     # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument(
            "app_path",
            help="Path to the application source directory.")
        parser.add_argument(
            "--build-root", default=None,
            help="Override the build root (default: <app_path>/build).")
        parser.add_argument(
            "--dry-run", action="store_true",
            help="Print the flash commands but don't run them.")
        return parser

    def do_run(self, args, _unknown):         # type: ignore[no-untyped-def]
        if find_sdk_root() is None:
            log.die("Cannot locate alp-sdk root.")
            return 1

        app_path = Path(args.app_path).resolve()
        build_root = (Path(args.build_root).resolve()
                      if args.build_root
                      else app_path / "build")
        manifest_path = build_root / "system-manifest.yaml"
        if not manifest_path.is_file():
            log.die(f"system-manifest.yaml not found at "
                    f"{manifest_path}; run `west alp-build "
                    f"{args.app_path}` first.")
            return 1

        manifest = yaml.safe_load(manifest_path.read_text(encoding="utf-8"))
        if not isinstance(manifest, dict):
            log.die(f"system-manifest.yaml at {manifest_path} did not "
                    f"parse to a mapping.")
            return 1

        # Build a map core_id -> slice entry for lookup.
        slices_by_core: dict[str, dict] = {
            s.get("core_id"): s for s in (manifest.get("slices") or [])
            if isinstance(s, dict) and s.get("core_id")
        }

        boot_order = list(manifest.get("boot_order") or [])
        if not boot_order:
            log.inf("alp-flash: boot_order is empty in the manifest; "
                    "flashing slices in alphabetical order")
            # Fall back to per-slice walk.
            steps = [{"stage": i + 1, "core": cid}
                     for i, cid in enumerate(sorted(slices_by_core.keys()))]
        else:
            steps = boot_order

        failed = 0
        for step in steps:
            core = step.get("core") if isinstance(step, dict) else None
            if not core:
                continue
            slice_ = slices_by_core.get(core)
            if not slice_:
                log.wrn(f"alp-flash: boot_order references core '{core}' "
                        f"not in slices; skipping")
                continue
            backend, cmd = _resolve_backend(slice_)
            tool = _BACKEND_TOOLS.get(backend, "")
            available = tool and shutil.which(tool) is not None
            if args.dry_run or not available:
                marker = "[DRY ]" if args.dry_run else "[STUB]"
                reason = "" if args.dry_run else f"  ({tool} not in PATH)"
                log.inf(f"{marker} flash {core} via {backend}: "
                        f"would run `{' '.join(cmd)}`{reason}")
                continue
            log.inf(f"alp-flash: programming {core} via {backend}")
            rv = _run_backend(cmd)
            if rv != 0:
                log.err(f"alp-flash: {core} backend exited {rv}")
                failed += 1
        return 1 if failed else 0


def _resolve_backend(slice_: dict) -> tuple[str, list[str]]:
    """Pick a backend for a slice.  Phase 2 returns a stub command;
    Phase 3+ wires real invocations.

    Returns (backend_name, command_list).
    """
    os_kind = slice_.get("os", "unknown")
    core_id = slice_.get("core_id", "unknown")
    artefact = slice_.get("output_artefact") or "<artefact>"
    if os_kind == "zephyr":
        # M-class slices typically flash via vendor SWD; real backend
        # land in Phase 3 once SoM-specific runners exist.
        return ("vendor-swd",
                ["echo", "stub: would JLinkExe load",
                 f"core={core_id}", f"file={artefact}"])
    if os_kind == "yocto":
        # A-cluster images flow through bitbake's deploy artefact -> a
        # vendor flash command (RZ Flash Writer, uuu for iMX, ...).
        return ("bitbake-flash",
                ["echo", "stub: would call vendor image-writer",
                 f"core={core_id}", f"file={artefact}"])
    if os_kind == "baremetal":
        return ("openocd",
                ["echo", "stub: would openocd program",
                 f"core={core_id}", f"file={artefact}"])
    return ("stub",
            ["echo", "stub: no backend known for",
             f"os={os_kind}", f"core={core_id}"])


def _run_backend(cmd: list[str]) -> int:
    import subprocess
    return subprocess.call(cmd)
