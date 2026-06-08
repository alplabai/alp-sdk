# SPDX-License-Identifier: Apache-2.0
"""xspi_flashwriter -- write a bootloader artefact (bl2 / fip .bin) to the
SoM's xSPI flash via the Renesas Flash Writer in SCIF download mode.

Backend for provisioning *blank* E1M-V2N modules, where no bootloader exists
yet to run `sf write` from U-Boot. The host puts the SoC in SCIF boot
(boot-ROM serial download), loads the Flash Writer (.mot) over the serial
port, then writes the artefact to the named flash partition (mtd0 = bl2,
mtd1 = fip).

Safety mirrors yocto_wic: even when ctx.dry_run is False, the backend only
performs real serial I/O when flash_args.confirm is true (or
ALP_FLASH_FORCE=1); otherwise it prints the planned sequence. The real SCIF
write is HW-gated and validated on the bench (currently shelved).

flash_args contract:
  flash_partition  str   "mtd0" (bl2) | "mtd1" (fip). REQUIRED.
  port             str?  Serial port to the SoM SCIF console ("COM24",
                         "/dev/ttyUSB0"). Required for a real write.
  flash_writer     str?  Path to Flash_Writer_SCIF_*.mot. Required for a real write.
  baud             int?  SCIF baud; default 115200.
  confirm          bool  Gate real serial I/O (default False => dry-run plan).
"""

from __future__ import annotations

import os
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register

_NAME = "xspi_flashwriter"


class XspiFlashWriter:
    """Renesas Flash Writer (SCIF) wrapper for blank-module bootloader flashing."""

    name: str = _NAME
    # pyserial is a Python module, not a PATH executable, so `requires` is empty;
    # the real path imports it lazily and reports clearly if it's absent.
    requires: list[str] = []

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        args = ctx.flash_args or {}
        partition = args.get("flash_partition")
        if partition not in ("mtd0", "mtd1"):
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("xspi_flashwriter: flash_args.flash_partition must be "
                         "'mtd0' (bl2) or 'mtd1' (fip)"))

        artefact = Path(ctx.artefact_path)
        port = args.get("port") or "<port>"
        writer = args.get("flash_writer") or "<flash_writer.mot>"
        baud = args.get("baud") or 115200
        plan = ["flash-writer-scif", f"port={port}", f"writer={writer}",
                f"baud={baud}", f"partition={partition}", f"artefact={artefact.name}"]

        confirm = bool(args.get("confirm")) or os.environ.get("ALP_FLASH_FORCE") == "1"
        if ctx.dry_run or not confirm:
            why = "dry-run" if ctx.dry_run else "flash_args.confirm is false"
            return FlashResult(
                ok=True, elapsed_s=time.monotonic() - start,
                message=(f"xspi_flashwriter[{ctx.core_id}]: would write "
                         f"{artefact.name} -> xSPI {partition} via Flash Writer "
                         f"on {port} ({why})"),
                command=plan)

        # Real path: HW-gated. Validate inputs, then hand to the SCIF writer.
        try:
            import serial  # noqa: F401  (pyserial)
        except ImportError:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("xspi_flashwriter: a real write needs pyserial "
                         "(`pip install pyserial`); use --dry-run otherwise"))
        if not Path(writer).is_file():
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=f"xspi_flashwriter: flash writer .mot not found at {writer}")
        return FlashResult(
            ok=False, elapsed_s=time.monotonic() - start,
            message=("xspi_flashwriter: the real SCIF write is HW-gated and not "
                     "yet validated on silicon (bench shelved). Run with --dry-run; "
                     "see docs/provisioning.md."),
            command=plan)


_INST = XspiFlashWriter()
register(_INST)

BACKEND: FlashBackend = _INST
