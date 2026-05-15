# SPDX-License-Identifier: Apache-2.0
"""
baremetal_cmake_flash -- invoke a CMake project's ``flash`` (or
``program``) target.

Backend invoked by ``west alp-flash`` for every slice whose
``flash_method`` is ``baremetal_cmake_flash``.  Baremetal slices are
typically vendor BSPs that ship a custom CMake target (``flash``,
``program``, ``erase_flash``); this backend just shells out to
``cmake --build <dir> --target <target>`` and lets the underlying
CMakeLists drive the actual programming.

flash_args contract:
  build_dir  str?   Path to the CMake build directory.  When absent,
                    derived from the artefact's parent.
  target     str?   CMake target to build; defaults to "flash".
                    Some BSPs use "program" / "flash_app" / "load".
  jobs       int?   Parallel jobs to pass to cmake (-j); default 1
                    (flash targets are usually serial anyway).
  config     str?   CMake config (``--config Release``); only used on
                    multi-config generators (VS, Xcode).
"""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


class BaremetalCmakeFlash:
    """`cmake --build <build_dir> --target <target>` wrapper."""

    name: str = "baremetal_cmake_flash"
    requires: list[str] = ["cmake"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        cmake = shutil.which("cmake")
        if cmake is None:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("baremetal_cmake_flash: `cmake` is not on PATH; "
                         "install CMake 3.20+ via the system package "
                         "manager or https://cmake.org/download/."),
            )

        build_dir = (ctx.flash_args or {}).get("build_dir")
        if not build_dir:
            artefact = Path(ctx.artefact_path)
            # Heuristic: the .elf/.bin sits in <build_dir>/ or a
            # subdir; walking up to find CMakeCache.txt would be more
            # robust but requires real disk access we don't want in
            # the unit-test path.
            build_dir = str(artefact.parent)
        target = (ctx.flash_args or {}).get("target") or "flash"

        cmd: list[str] = [cmake, "--build", str(build_dir),
                          "--target", str(target)]

        config = (ctx.flash_args or {}).get("config")
        if config:
            cmd.extend(["--config", str(config)])

        jobs = (ctx.flash_args or {}).get("jobs")
        if jobs:
            cmd.extend(["-j", str(jobs)])

        if ctx.dry_run:
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"baremetal_cmake_flash[{ctx.core_id}]: would run "
                        f"{' '.join(cmd)} (dry-run)",
                command=list(cmd),
            )

        proc = subprocess.run(cmd, check=False,
                              capture_output=True, text=True)
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            return FlashResult(
                ok=True,
                elapsed_s=elapsed,
                message=f"baremetal_cmake_flash[{ctx.core_id}]: target "
                        f"`{target}` ok in {elapsed:.1f}s",
                command=list(cmd),
            )
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False,
            elapsed_s=elapsed,
            message=(f"baremetal_cmake_flash[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=list(cmd),
        )


_INST = BaremetalCmakeFlash()
register(_INST)

BACKEND: FlashBackend = _INST
