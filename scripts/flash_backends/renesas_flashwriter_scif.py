# SPDX-License-Identifier: Apache-2.0
"""renesas_flashwriter_scif -- plan a bootloader write to a blank E1M-V2N
module through the boot ROM's SCIF download mode and the Renesas Flash Writer.

A blank module has no bootloader to run `sf write` or `mmc write` from, so the
host puts the SoC in SCIF download mode, loads the Flash Writer (.mot) over
the serial port and lets it do the write.

Targets (``flash_args.flash_partition``): ``mtd0`` / ``mtd1`` (xSPI bl2 / fip)
and ``emmc:boot1`` (eMMC boot partition 1).

This backend only PLANS. A confirmed write is refused: the V2N flow
(``provision_som.py plan|run``, docs/provisioning-v2n.md) drives the Flash
Writer itself for the eMMC boot1 bootstrap (``provision.scif_writer``) and
writes xSPI from Linux; the Flash Writer's XLS2 xSPI dialog is not driven.

flash_args contract:
  flash_partition  str   "emmc:boot1" | "mtd0" | "mtd1". REQUIRED.
  port             str?  Serial port to the SoM SCIF console (plan text only).
  flash_writer     str?  Path to the Flash Writer .mot (plan text only).
  baud             int?  SCIF baud; default 115200.
  confirm          bool  A real write was asked for => refused.
"""

from __future__ import annotations

import os
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register

_NAME = "renesas_flashwriter_scif"
_TARGETS = ("emmc:boot1", "mtd0", "mtd1")


class RenesasFlashWriterScif:
    """Renesas Flash Writer (SCIF) plan for blank-module bootloader flashing."""

    name: str = _NAME
    requires: list[str] = []

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()

        def result(ok: bool, message: str, command: list[str] | None = None) -> FlashResult:
            return FlashResult(ok=ok, elapsed_s=time.monotonic() - start,
                               message=f"{self.name}[{ctx.core_id}]: {message}",
                               command=command or [])

        args = ctx.flash_args or {}
        target = args.get("flash_partition")
        if target not in _TARGETS:
            return result(False, "flash_args.flash_partition must be one of "
                                 + ", ".join(repr(t) for t in _TARGETS))
        artefact = Path(ctx.artefact_path)
        port = args.get("port") or "<port>"
        writer = args.get("flash_writer") or "<flash_writer.mot>"
        baud = int(args.get("baud") or 115200)
        where = "eMMC boot1" if target == "emmc:boot1" else f"xSPI {target}"
        plan = ["flash-writer-scif", f"port={port}", f"writer={writer}", f"baud={baud}",
                f"partition={target}", f"artefact={artefact.name}"]
        confirm = bool(args.get("confirm")) or os.environ.get("ALP_FLASH_FORCE") == "1"
        if ctx.dry_run or not confirm:
            why = "dry-run" if ctx.dry_run else "flash_args.confirm is false"
            return result(True, f"would write {artefact.name} -> {where} via Flash Writer "
                                f"on {port} ({why})", plan)
        return result(False, "a real Flash Writer write is not implemented in this backend; "
                             "use the V2N flow (provision_som.py run, docs/provisioning-v2n.md)",
                      plan)


BACKEND: FlashBackend = RenesasFlashWriterScif()
register(BACKEND)
