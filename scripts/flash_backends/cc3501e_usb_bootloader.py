# SPDX-License-Identifier: Apache-2.0
"""
cc3501e_usb_bootloader -- flash the CC3501E Wi-Fi/BLE coprocessor
via its USB-CDC bootloader.

Backend invoked by ``west alp-flash`` for every helper-MCU entry
whose ``flash_method`` is ``cc3501e_usb_bootloader``.

----------------------------------------------------------------------
Implementation status: SHAPE COMPLETE -- VENDOR CLI NOT FINALISED.

TI's CC3501E (SimpleLink Wi-Fi 7) is documented in SWRU626 with an
internal serial bootloader, but the public-facing flashing tool's CLI
isn't documented in a stable form as of 2026-05.  ROM bootloader
protocol is documented (SWRU626 §6) but the canonical host-side
wrapper -- ``cc3501e-flasher`` -- isn't on a public TI mirror yet.

This backend's surface (name, requires, flash_args contract) is the
final shape per the §5.7 host integration plan in
``docs/cc3501e-integration-plan.md``; the body raises
``NotImplementedError`` with the TODO reference so a caller routing
through here gets an honest signal rather than a fake "ok".

Dry-runs still work: when ``ctx.dry_run`` is True the backend prints
the command it WOULD issue once the upstream CLI lands, which is
useful for manifest validation in CI without a real device.
----------------------------------------------------------------------

flash_args contract:
  device     str    Host-side serial device path -- "/dev/ttyACM0",
                    "COM3".  REQUIRED.
  mode       str    "otp_program" -- one-shot OTP burn (permanent).
                    "ram_load"    -- volatile load into SRAM for
                                     bring-up; image lost on reset.
                    REQUIRED.
  speed      int?   Bootloader UART speed in baud; default 921600.
                    The ROM bootloader auto-syncs on the AA-55 seed
                    so any sane baud rate works.
  verify     bool   Read-back verification after programming.
                    Defaults to True for otp_program (one-shot, must
                    be right) and False for ram_load.
"""

from __future__ import annotations

import shutil
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


# Vendor tool name candidates, in preference order.  When TI ships
# the public CLI we set the first one that resolves on PATH.
_CANDIDATE_TOOLS = ["cc3501e-flasher", "cc3501e-tool"]


def _resolve_tool() -> str | None:
    for tool in _CANDIDATE_TOOLS:
        path = shutil.which(tool)
        if path:
            return path
    return None


class Cc3501eUsbBootloaderFlash:
    """USB-CDC bootloader wrapper for the TI CC3501E Wi-Fi/BLE chip."""

    name: str = "cc3501e_usb_bootloader"
    # Either candidate is acceptable; the dispatcher treats `requires`
    # as "at least one of these on PATH".  See alp_flash.py.
    requires: list[str] = list(_CANDIDATE_TOOLS)

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()

        device = (ctx.flash_args or {}).get("device") or ""
        mode = (ctx.flash_args or {}).get("mode") or ""
        if not device or not mode:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("cc3501e_usb_bootloader: flash_args.device and "
                         "flash_args.mode are required (mode: "
                         "otp_program | ram_load)."),
            )
        if mode not in ("otp_program", "ram_load"):
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=(f"cc3501e_usb_bootloader: unknown mode '{mode}' "
                         f"-- expected otp_program | ram_load."),
            )

        tool = _resolve_tool()
        if tool is None:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=(
                    "cc3501e_usb_bootloader: no CC3501E host tool on "
                    "PATH.  Looked for: "
                    + ", ".join(_CANDIDATE_TOOLS)
                    + ".  See docs/cc3501e-integration-plan.md §5.7 "
                      "for the bring-up procedure once TI publishes the CLI."
                ),
            )

        speed = int((ctx.flash_args or {}).get("speed") or 921600)
        verify = (ctx.flash_args or {}).get("verify")
        if verify is None:
            verify = (mode == "otp_program")

        cmd: list[str] = [
            tool,
            "--device", str(device),
            "--mode", str(mode),
            "--speed", str(speed),
            "--image", str(ctx.artefact_path),
        ]
        if verify:
            cmd.append("--verify")

        if ctx.dry_run:
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"cc3501e_usb_bootloader[{ctx.core_id}]: would run "
                        f"{' '.join(cmd)} (dry-run; CLI shape provisional)",
                command=list(cmd),
            )

        # The CLI shape above is the intended one per the §5.7 design
        # doc, but the upstream tool isn't shipping yet.  Return a
        # clean failure rather than raising so west alp-flash can
        # report the skip gracefully instead of crashing.
        #
        # TODO(cc3501e §5.7): once TI ships cc3501e-flasher and the
        # wire-level CLI shape is validated, drop this early return and
        # let the subprocess.run path live.
        return FlashResult(
            ok=False,
            elapsed_s=time.monotonic() - start,
            message=("cc3501e-flasher CLI is not yet public; this backend "
                     "lands when the upstream tool stabilises.  Use the "
                     "manual SWD path documented in docs/bring-up-aen.md "
                     "for now."),
        )


_INST = Cc3501eUsbBootloaderFlash()
register(_INST)

BACKEND: FlashBackend = _INST
