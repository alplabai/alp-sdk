# SPDX-License-Identifier: Apache-2.0
"""
swd_v2n_host -- flash the GD32G553 supervisor MCU via the V2N host's
SWD pins.

On E1M-V2N101 the V2N drives SWDIO=P70, SWCLK=P71, NRST=P74 to the
GD32 (see project memory: "V2N→GD32 SWD + NRST assigned").  This
backend wraps either ``openocd`` (preferred -- the alp-sdk ships an
OpenOCD interface config under scripts/openocd/cmsis-dap.cfg) or
``pyocd`` as a fallback for hosts without OpenOCD.

flash_args contract:
  interface  str    OpenOCD interface ID -- "cmsis-dap" | "jlink" |
                    "stlink".  REQUIRED.
  target     str    OpenOCD target ID -- usually "gd32g553" or a
                    nested family name resolved from the SoM preset.
                    REQUIRED.
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


class SwdV2nHostFlash:
    """OpenOCD-via-CMSIS-DAP / pyocd wrapper for the GD32G553."""

    name: str = "swd_v2n_host"
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
                message=("swd_v2n_host: flash_args.interface and "
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
                message=("swd_v2n_host: neither `openocd` nor `pyocd` is "
                         "on PATH; install openocd (preferred) via "
                         "`apt install openocd` / `brew install openocd` "
                         "or `pip install pyocd`."),
            )

        if ctx.dry_run:
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"swd_v2n_host[{ctx.core_id}]: would run "
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
                message=f"swd_v2n_host[{ctx.core_id}]: GD32G553 flashed "
                        f"@ {base} in {elapsed:.1f}s",
                command=list(cmd),
            )
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False,
            elapsed_s=elapsed,
            message=(f"swd_v2n_host[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=list(cmd),
        )


_INST = SwdV2nHostFlash()
register(_INST)

BACKEND: FlashBackend = _INST
