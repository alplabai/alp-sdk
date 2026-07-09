# SPDX-License-Identifier: Apache-2.0
"""
yocto_wic -- flash a `.wic` / `.wic.gz` / `.wic.xz` image to an SD
card or eMMC raw device.

Backend invoked by ``west alp-flash`` for every slice whose
``flash_method`` is ``yocto_wic_to_sd_or_emmc`` (the orchestrator's
canonical name) or its short alias ``yocto_wic``.

Tool selection:
  * Prefer ``bmaptool`` -- sparse-aware, ~3x faster on Yocto images
    that ship with a side-car ``.bmap``.
  * Fall back to ``dd`` (with ``gunzip -c`` / ``xz -dc`` pre-decompress
    when the artefact is compressed).

Safety: refuses any ``target`` that doesn't start with ``/dev/`` so a
stray ``flash_args.target: /tmp/spill.img`` never silently overwrites
a developer's tmp file.  An explicit ``flash_args.confirm: true`` (or
``ALP_FLASH_FORCE=1`` env override) is required to actually run the
sub-process; without it, the backend dry-runs even when ``ctx.dry_run``
is False -- protects against accidental ``west alp-flash`` against a
mounted system disk.

flash_args contract:
  target     str    Destination device path ("/dev/sdb", "/dev/mmcblk0").
                    REQUIRED.  Must start with "/dev/".
  compress   str?   "gz" | "xz" | None.  Optional; auto-detected from the
                    artefact suffix when omitted.
  confirm    bool   When True, the backend actually invokes the
                    underlying tool.  When False (default), the
                    backend treats the call as a dry-run.
  bs         str?   ``dd`` block size; default "4M".
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


_NAME = "yocto_wic_to_sd_or_emmc"
_ALIAS = "yocto_wic"


class YoctoWicFlash:
    """bmaptool / dd wrapper for `.wic` images."""

    name: str = _NAME
    requires: list[str] = ["bmaptool", "dd"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        target = (ctx.flash_args or {}).get("target") or ""
        if not target:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("yocto_wic: flash_args.target is required "
                         "(e.g. /dev/sdb)"),
            )
        if not str(target).startswith("/dev/"):
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=(f"yocto_wic: refusing target '{target}' -- must "
                         f"start with /dev/ to avoid clobbering a regular "
                         f"file.  Set flash_args.target to a real block "
                         f"device."),
            )

        artefact = Path(ctx.artefact_path)
        compress = (ctx.flash_args or {}).get("compress")
        if compress is None:
            sx = artefact.suffix.lower()
            if sx == ".gz":
                compress = "gz"
            elif sx == ".xz":
                compress = "xz"

        # Tool selection: bmaptool wins when present + the artefact is a
        # plain .wic (bmaptool transparently handles .gz / .xz too).
        bmaptool = shutil.which("bmaptool")
        dd = shutil.which("dd")

        confirm = bool((ctx.flash_args or {}).get("confirm")) or \
            os.environ.get("ALP_FLASH_FORCE") == "1"
        # Only a confirmed, non-dry-run flash actually shells out -- a
        # dry-run (or an unconfirmed call) just reports the plan, so it
        # must not require bmaptool/dd to be installed on this host.
        will_execute = confirm and not ctx.dry_run

        if not bmaptool and not dd and will_execute:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("yocto_wic: neither `bmaptool` nor `dd` is on "
                         "PATH; install bmaptool (preferred -- sparse "
                         "aware) via `apt install bmap-tools` or run on "
                         "a host with coreutils."),
            )

        bs = (ctx.flash_args or {}).get("bs") or "4M"

        if bmaptool or (not dd and not will_execute):
            # Either bmaptool is really present, or neither tool is on
            # PATH and this is only a plan (dry-run / unconfirmed) --
            # show the preferred (bmaptool) shape of the command.
            tool = bmaptool or "bmaptool"
            cmd = [tool, "copy", str(artefact), str(target)]
        elif compress == "gz":
            cmd = ["sh", "-c",
                   f"gunzip -c {shlex.quote(str(artefact))} | "
                   f"dd of={shlex.quote(str(target))} "
                   f"bs={shlex.quote(str(bs))} conv=fsync status=progress"]
        elif compress == "xz":
            cmd = ["sh", "-c",
                   f"xz -dc {shlex.quote(str(artefact))} | "
                   f"dd of={shlex.quote(str(target))} "
                   f"bs={shlex.quote(str(bs))} conv=fsync status=progress"]
        else:
            dd_bin = dd or "dd"
            cmd = [dd_bin, f"if={artefact}", f"of={target}", f"bs={bs}",
                   "conv=fsync", "status=progress"]

        if ctx.dry_run or not confirm:
            why = ("dry-run" if ctx.dry_run
                   else "flash_args.confirm is false (set ALP_FLASH_FORCE=1 "
                        "or flash_args.confirm: true to actually run)")
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"yocto_wic[{ctx.core_id}]: would run {' '.join(cmd)} ({why})",
                command=list(cmd),
            )

        proc = subprocess.run(cmd, check=False,
                              capture_output=True, text=True)
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            return FlashResult(
                ok=True,
                elapsed_s=elapsed,
                message=f"yocto_wic[{ctx.core_id}]: programmed {target} "
                        f"in {elapsed:.1f}s",
                command=list(cmd),
            )
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False,
            elapsed_s=elapsed,
            message=(f"yocto_wic[{ctx.core_id}]: exited rc={proc.returncode} "
                     f"-- {tail_msg}"),
            command=list(cmd),
        )


# Self-register under the canonical name + short alias.
_INST = YoctoWicFlash()
register(_INST)


class _YoctoWicAlias(YoctoWicFlash):
    """Alias registration so ``yocto_wic`` resolves to the same logic.

    The orchestrator emits ``yocto_wic_to_sd_or_emmc``; some hand-edited
    manifests use the shorter ``yocto_wic`` -- both work.
    """

    name: str = _ALIAS


register(_YoctoWicAlias())


# Module-level handle exposed for callers that want to import the
# instance directly (mostly tests).
BACKEND: FlashBackend = _INST
