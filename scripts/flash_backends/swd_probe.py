# SPDX-License-Identifier: Apache-2.0
"""
swd_probe -- flash a Cortex-M target over SWD with an external probe.

Used for the GD32G553 supervisor on E1M-X V2N / V2N-M1: the probe connects
to the GD32's SWD header (SWDIO=PA13, SWCLK=PA14, NRST) and programs it
directly (see metadata/chips/gd32g553.yaml).

Primary path: SEGGER J-Link (`JLink -device GD32G553MEY7TR`; requires J-Link
software >= V9.46), which has built-in GD32G553 flash support. OpenOCD / pyOCD
are fallbacks for CMSIS-DAP / ST-Link users (mainline OpenOCD's GD32G5 flash
support is version-dependent -- see docs/gd32-flashing.md + scripts/openocd/).

flash_args contract:
  interface    str   OpenOCD interface ID ("cmsis-dap" | "jlink" | "stlink").
                     Required for the openocd/pyocd path; ignored by J-Link.
  target       str   OpenOCD target ID ("gd32g553"). Required for the
                     openocd/pyocd path; ignored by J-Link.
  base         str?  Flash base address (default "0x08000000").
  reset        bool  Reset+run after programming (default True).
  use_pyocd    bool  Force the pyocd path (skip J-Link + openocd).
  use_openocd  bool  Force the openocd path (skip J-Link auto-pick).
  jlink_device str?  SEGGER device name (default "GD32G553MEY7TR").
  jlink_speed  int?  SWD speed in kHz (default 4000).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


_DEFAULT_BASE = "0x08000000"
_DEFAULT_JLINK_DEVICE = "GD32G553MEY7TR"
_DEFAULT_JLINK_SPEED = 4000
_JLINK_BINARIES = ("JLinkExe", "JLink")     # "JLinkExe" on Linux/macOS, "JLink" on Windows -- try both


def _jlink_commander_script(artefact: Path, base: str, do_reset: bool) -> str:
    """Build the J-Link Commander script that programs ``artefact``.

    ``.bin`` images need an explicit load address; ``.hex`` / ``.elf``
    carry their own addresses so ``loadfile`` suffices.
    """
    path = str(artefact)
    lines = ["r", "halt"]
    if artefact.suffix.lower() == ".bin":
        lines.append(f"loadbin {path}, {base}")
    else:
        lines.append(f"loadfile {path}")
    if do_reset:
        lines += ["r", "g"]
    lines.append("qc")
    return "\n".join(lines) + "\n"


def _which_jlink() -> "str | None":
    for name in _JLINK_BINARIES:
        found = shutil.which(name)
        if found:
            return found
    return None


class SwdProbeFlash:
    """SEGGER J-Link (primary) / OpenOCD / pyOCD external-probe flasher."""

    name: str = "swd_probe"
    requires: list[str] = list(_JLINK_BINARIES) + ["openocd", "pyocd"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        fa = ctx.flash_args or {}
        base = fa.get("base") or _DEFAULT_BASE
        do_reset = bool(fa.get("reset", True))
        force_pyocd = bool(fa.get("use_pyocd"))
        force_openocd = bool(fa.get("use_openocd"))

        # ---- J-Link (primary; best GD32G5 flash support) ----
        jlink = None if (force_pyocd or force_openocd) else _which_jlink()
        if jlink:
            device = fa.get("jlink_device") or _DEFAULT_JLINK_DEVICE
            speed = str(fa.get("jlink_speed") or _DEFAULT_JLINK_SPEED)
            script = _jlink_commander_script(ctx.artefact_path, base, do_reset)
            base_cmd = [
                jlink, "-device", device, "-if", "SWD", "-speed", speed,
                "-AutoConnect", "1", "-ExitOnError", "1", "-NoGui", "1",
                "-CommanderScript",
            ]
            if ctx.dry_run:
                cmd = base_cmd + ["<generated.jlink>"]
                return FlashResult(
                    ok=True,
                    elapsed_s=time.monotonic() - start,
                    message=(f"swd_probe[{ctx.core_id}]: would run "
                             f"{' '.join(cmd)} (dry-run); J-Link script:\n"
                             f"{script}"),
                    command=cmd,
                )
            fd, script_path = tempfile.mkstemp(suffix=".jlink")
            try:
                with os.fdopen(fd, "w") as fh:
                    fh.write(script)
                cmd = base_cmd + [script_path]
                proc = subprocess.run(cmd, check=False,
                                      capture_output=True, text=True)
            finally:
                try:
                    os.unlink(script_path)
                except OSError:                      # pragma: no cover
                    pass
            elapsed = time.monotonic() - start
            if proc.returncode == 0:
                return FlashResult(
                    ok=True, elapsed_s=elapsed,
                    message=(f"swd_probe[{ctx.core_id}]: GD32G553 flashed "
                             f"via J-Link ({device}) @ {base} in "
                             f"{elapsed:.1f}s"),
                    command=cmd)
            tail = (proc.stderr or proc.stdout or "").strip().splitlines()
            tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
            return FlashResult(
                ok=False, elapsed_s=elapsed,
                message=(f"swd_probe[{ctx.core_id}]: J-Link exited "
                         f"rc={proc.returncode} -- {tail_msg}"),
                command=cmd)

        # ---- OpenOCD / pyOCD (alternative; need interface + target) ----
        interface = fa.get("interface") or ""
        target = fa.get("target") or ""
        if not interface or not target:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("swd_probe: flash_args.interface and "
                         "flash_args.target are required for the openocd/pyocd "
                         "path (e.g. interface=cmsis-dap, target=gd32g553) -- "
                         "or install SEGGER J-Link for the primary path."))

        openocd = None if force_pyocd else shutil.which("openocd")
        pyocd = shutil.which("pyocd")

        if openocd:
            program_cmd = f"program {ctx.artefact_path} verify"
            if do_reset:
                program_cmd += " reset"
            program_cmd += f" exit {base}"
            cmd = [openocd, "-f", f"interface/{interface}.cfg",
                   "-f", f"target/{target}.cfg", "-c", program_cmd]
        elif pyocd:
            cmd = [pyocd, "flash", "--target", str(target),
                   "--base-address", str(base), str(ctx.artefact_path)]
        else:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("swd_probe: no flash tool found -- install SEGGER "
                         "J-Link (preferred), or `openocd`, or `pyocd`."))

        if ctx.dry_run:
            return FlashResult(
                ok=True, elapsed_s=time.monotonic() - start,
                message=f"swd_probe[{ctx.core_id}]: would run "
                        f"{' '.join(cmd)} (dry-run)",
                command=cmd)

        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            return FlashResult(
                ok=True, elapsed_s=elapsed,
                message=f"swd_probe[{ctx.core_id}]: GD32G553 flashed "
                        f"@ {base} in {elapsed:.1f}s",
                command=cmd)
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False, elapsed_s=elapsed,
            message=(f"swd_probe[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=cmd)


_INST = SwdProbeFlash()
register(_INST)

BACKEND: FlashBackend = _INST
