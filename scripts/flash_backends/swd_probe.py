# SPDX-License-Identifier: Apache-2.0
"""
swd_probe -- flash a Cortex-M target over SWD with an external probe.

Used for the GD32G553 supervisor on E1M-X boards: the probe connects
to the GD32's SWD header (SWDIO=PA13, SWCLK=PA14, NRST) and programs
it directly (see metadata/chips/gd32g553.yaml).

flash_args contract:
  interface  str    OpenOCD interface ID -- "cmsis-dap" | "jlink" |
                    "stlink".  REQUIRED for openocd/pyocd path.
  target     str    OpenOCD target ID -- usually "gd32g553" or a
                    nested family name resolved from the SoM preset.
                    REQUIRED for openocd/pyocd path.
  base       str?   Flash base address as a hex string ("0x08000000").
                    Defaults to "0x08000000" (GD32G553 main flash).
  reset      bool   Issue ``reset run`` after programming.  Defaults
                    to True; only set False for debug sessions where
                    you'll attach a debugger immediately afterwards.
  use_pyocd  bool   Force the pyocd code path even when openocd is
                    available.  Mainly for CI to exercise both paths.
"""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


_DEFAULT_BASE = "0x08000000"


class SwdProbeFlash:
    """OpenOCD-via-CMSIS-DAP / pyocd wrapper for the GD32G553."""

    name: str = "swd_probe"
    requires: list[str] = ["openocd", "pyocd"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()

        interface = (ctx.flash_args or {}).get("interface") or ""
        target = (ctx.flash_args or {}).get("target") or ""
        base = (ctx.flash_args or {}).get("base") or _DEFAULT_BASE
        do_reset = bool((ctx.flash_args or {}).get("reset", True))
        force_pyocd = bool((ctx.flash_args or {}).get("use_pyocd"))

        if not interface or not target:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("swd_probe: flash_args.interface and "
                         "flash_args.target are required (e.g. "
                         "interface=cmsis-dap, target=gd32g553)."),
            )

        openocd = None if force_pyocd else shutil.which("openocd")
        pyocd = shutil.which("pyocd")

        if openocd:
            # `program <image> verify reset exit <base>` -- canonical
            # OpenOCD flash recipe.  `reset` triggers `reset run`
            # after verification, so the GD32 boots into the freshly
            # flashed firmware.
            program_cmd = f"program {ctx.artefact_path} verify"
            if do_reset:
                program_cmd += " reset"
            program_cmd += f" exit {base}"
            cmd = [
                openocd,
                "-f", f"interface/{interface}.cfg",
                "-f", f"target/{target}.cfg",
                "-c", program_cmd,
            ]
        elif pyocd:
            # pyocd flash <image> --target <target> --base-address <base>
            # The CMSIS-DAP / J-Link interface is auto-detected; we
            # don't pass --probe so users can swap probes without
            # touching the manifest.
            cmd = [
                pyocd, "flash",
                "--target", str(target),
                "--base-address", str(base),
                str(ctx.artefact_path),
            ]
        else:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("swd_probe: neither `openocd` nor `pyocd` is "
                         "on PATH; install openocd (preferred) via "
                         "`apt install openocd` / `brew install openocd` "
                         "or `pip install pyocd`."),
            )

        if ctx.dry_run:
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"swd_probe[{ctx.core_id}]: would run "
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
                message=f"swd_probe[{ctx.core_id}]: GD32G553 flashed "
                        f"@ {base} in {elapsed:.1f}s",
                command=list(cmd),
            )
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False,
            elapsed_s=elapsed,
            message=(f"swd_probe[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=list(cmd),
        )


_INST = SwdProbeFlash()
register(_INST)

BACKEND: FlashBackend = _INST
