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

Safety: canonicalizes ``target`` (resolves ``..`` traversal and any
symlink) and refuses anything that doesn't resolve strictly beneath
``/dev/`` -- so a stray ``flash_args.target: /tmp/spill.img``, a
``/dev/../tmp/spill.img`` traversal, or a ``/dev/foo`` symlink pointing
outside ``/dev`` never silently overwrites a developer's file.  This
root check runs unconditionally, including for a dry-run preview of a
target that isn't plugged in yet.  Right before a tool is actually
about to be invoked (i.e. not during a dry-run/planning-only preview),
a second, stricter gate additionally requires the resolved target to
``stat`` as a real block device -- regular files are never a valid
flash target for this backend, and there is no silent fallback.  An
explicit ``flash_args.confirm: true`` (or ``ALP_FLASH_FORCE=1`` env
override) is required to actually run the sub-process; without it, the
backend dry-runs even when ``ctx.dry_run`` is False -- protects against
accidental ``west alp-flash`` against a mounted system disk.

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
import stat
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


_NAME = "yocto_wic_to_sd_or_emmc"
_ALIAS = "yocto_wic"

# The directory a flash target must resolve beneath.  A module-level
# constant (rather than a hardcoded literal in the check) so tests can
# point it at a tmp_path sandbox and exercise the real resolution logic
# against real files/symlinks without touching the host's actual /dev.
_DEV_ROOT = "/dev"


def _resolve_dev_root(target: str) -> "tuple[Path | None, str | None]":
    """Canonicalize ``target`` (resolve ``..`` traversal and any symlink
    chain) and require the result to stay strictly beneath ``_DEV_ROOT``.

    Fails closed against traversal and symlink escapes.  Does not touch
    ``stat`` and does not require the target to exist -- ``os.path.
    realpath`` normalizes lexically for path components that aren't
    there, so this is safe to run unconditionally, including for a
    dry-run preview of a target that isn't plugged in yet.  The
    existence + block-device check is a separate, stricter gate (see
    ``_require_block_device``) applied only right before a tool is
    actually about to be invoked.

    Returns ``(resolved_path, None)`` on success or ``(None, message)``
    on rejection.
    """
    root = Path(_DEV_ROOT)
    resolved = Path(os.path.realpath(target))
    try:
        resolved.relative_to(root)
    except ValueError:
        return None, (
            f"yocto_wic: refusing target '{target}' -- resolves to "
            f"'{resolved}', which is not beneath {root}/.  Set "
            f"flash_args.target to a real block device under {root}/."
        )
    if resolved == root:
        return None, (
            f"yocto_wic: refusing target '{target}' -- resolves to "
            f"{root} itself, not a device beneath it."
        )
    return resolved, None


def _require_block_device(resolved: Path) -> "str | None":
    """Require ``resolved`` (already root-checked by
    ``_resolve_dev_root``) to ``stat`` as a real block device.

    Regular files are never a valid flash target for this backend --
    there is no silent fallback.  Called only when a tool is actually
    about to be invoked (not during a dry-run/planning preview), so it
    never blocks previewing a target that isn't physically present yet.

    Returns ``None`` on success or an error message on rejection.
    """
    try:
        mode = os.stat(resolved).st_mode
    except OSError as exc:
        return (
            f"yocto_wic: refusing target '{resolved}' -- cannot stat "
            f"it: {exc}."
        )
    if not stat.S_ISBLK(mode):
        return (
            f"yocto_wic: refusing target '{resolved}' -- not a block "
            f"device.  Regular files are not a supported flash target "
            f"for this backend."
        )
    return None


@dataclass
class _ProcOutcome:
    returncode: int
    stdout: str = ""
    stderr: str = ""


def _cmd_display(cmd: list[str]) -> str:
    return shlex.join(cmd)


def _pipeline_display(decompress_cmd: list[str], dd_cmd: list[str]) -> str:
    return f"{_cmd_display(decompress_cmd)} | {_cmd_display(dd_cmd)}"


def _gzip_cmd(artefact: Path) -> list[str] | None:
    gunzip = shutil.which("gunzip")
    if gunzip:
        return [gunzip, "-c", str(artefact)]
    gzip = shutil.which("gzip")
    if gzip:
        return [gzip, "-dc", str(artefact)]
    return None


def _run_pipeline(decompress_cmd: list[str], dd_cmd: list[str]) -> _ProcOutcome:
    with tempfile.TemporaryFile() as decompress_stderr:
        decompressor = subprocess.Popen(
            decompress_cmd,
            stdout=subprocess.PIPE,
            stderr=decompress_stderr,
        )
        stdout_pipe = decompressor.stdout
        if stdout_pipe is None:                      # pragma: no cover
            return _ProcOutcome(1, stderr="decompressor stdout pipe unavailable")
        try:
            dd_proc = subprocess.run(
                dd_cmd,
                stdin=stdout_pipe,
                check=False,
                capture_output=True,
                text=True,
            )
        finally:
            stdout_pipe.close()
        decompress_rc = decompressor.wait()

        decompress_stderr.seek(0)
        decompress_err = decompress_stderr.read().decode("utf-8", errors="replace")

    stderr = "\n".join(part for part in (decompress_err, dd_proc.stderr or "") if part)
    return _ProcOutcome(
        returncode=dd_proc.returncode if dd_proc.returncode != 0 else decompress_rc,
        stdout=dd_proc.stdout or "",
        stderr=stderr,
    )


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
        resolved_target, target_err = _resolve_dev_root(str(target))
        if target_err:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=target_err,
            )
        target = str(resolved_target)

        artefact = Path(ctx.artefact_path)
        compress = (ctx.flash_args or {}).get("compress")
        if compress is None:
            sx = artefact.suffix.lower()
            if sx == ".gz":
                compress = "gz"
            elif sx == ".xz":
                compress = "xz"

        confirm = bool((ctx.flash_args or {}).get("confirm")) or \
            os.environ.get("ALP_FLASH_FORCE") == "1"
        planning_only = ctx.dry_run or not confirm

        # Tool selection: bmaptool wins when present + the artefact is a
        # plain .wic (bmaptool transparently handles .gz / .xz too).
        bmaptool = shutil.which("bmaptool")
        dd = shutil.which("dd")

        if bmaptool or (planning_only and dd is None):
            bmaptool_cmd = bmaptool or "bmaptool"
            cmd = [bmaptool_cmd, "copy", str(artefact), str(target)]
            command_display = _cmd_display(cmd)
            pipeline: tuple[list[str], list[str]] | None = None
        elif dd:
            bs = (ctx.flash_args or {}).get("bs") or "4M"
            if compress == "gz":
                decompress_cmd = _gzip_cmd(artefact)
                if decompress_cmd is None:
                    return FlashResult(
                        ok=False,
                        elapsed_s=time.monotonic() - start,
                        message=("yocto_wic: compressed .wic.gz fallback needs "
                                 "`gunzip` or `gzip` on PATH."),
                    )
                dd_cmd = [dd, f"of={target}", f"bs={bs}",
                          "conv=fsync", "status=progress"]
                cmd = decompress_cmd + ["|"] + dd_cmd
                command_display = _pipeline_display(decompress_cmd, dd_cmd)
                pipeline = (decompress_cmd, dd_cmd)
            elif compress == "xz":
                xz = shutil.which("xz")
                if xz is None:
                    return FlashResult(
                        ok=False,
                        elapsed_s=time.monotonic() - start,
                        message=("yocto_wic: compressed .wic.xz fallback needs "
                                 "`xz` on PATH."),
                    )
                decompress_cmd = [xz, "-dc", str(artefact)]
                dd_cmd = [dd, f"of={target}", f"bs={bs}",
                          "conv=fsync", "status=progress"]
                cmd = decompress_cmd + ["|"] + dd_cmd
                command_display = _pipeline_display(decompress_cmd, dd_cmd)
                pipeline = (decompress_cmd, dd_cmd)
            else:
                cmd = [dd, f"if={artefact}", f"of={target}", f"bs={bs}",
                       "conv=fsync", "status=progress"]
                command_display = _cmd_display(cmd)
                pipeline = None
        else:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("yocto_wic: neither `bmaptool` nor `dd` is on "
                         "PATH; install bmaptool (preferred -- sparse "
                         "aware) via `apt install bmap-tools` or run on "
                         "a host with coreutils."),
            )

        if planning_only:
            why = ("dry-run" if ctx.dry_run
                   else "flash_args.confirm is false (set ALP_FLASH_FORCE=1 "
                        "or flash_args.confirm: true to actually run)")
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"yocto_wic[{ctx.core_id}]: would run {command_display} ({why})",
                command=list(cmd),
            )

        # Fail closed right before touching real hardware: the target
        # must actually be a block device beneath /dev/, not merely
        # resolve there lexically.  Checked here (not up front) so a
        # dry-run/planning preview of a not-yet-connected target still
        # works.
        block_err = _require_block_device(resolved_target)
        if block_err:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=block_err,
            )

        if pipeline is not None:
            proc = _run_pipeline(*pipeline)
        else:
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
