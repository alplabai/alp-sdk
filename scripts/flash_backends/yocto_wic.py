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

Safety: canonicalizes ``target`` (lexical ``..`` traversal collapse on
every host; additionally chases symlinks via ``os.path.realpath`` on
POSIX hosts) and refuses anything that doesn't resolve strictly beneath
``/dev/`` -- so a stray ``flash_args.target: /tmp/spill.img``, a
``/dev/../tmp/spill.img`` traversal, or (on POSIX) a ``/dev/foo``
symlink pointing outside ``/dev`` never silently overwrites a
developer's file.  The command actually run always uses the caller's
original ``target`` string, never the resolved/canonicalized form --
the resolution above is for validation and the block-device stat check
only.  This root check runs unconditionally, including for a dry-run
preview of a target that isn't plugged in yet.  Right before a tool is
actually about to be invoked (i.e. not during a dry-run/planning-only
preview), a second, stricter gate additionally requires the resolved
target to ``stat`` as a real block device -- regular files are never a
valid flash target for this backend, and there is no silent fallback.
That stat and the later ``dd``/``bmaptool`` open-by-name are not
atomic (TOCTOU): a target swapped out from under the process between
the two would slip past the check.  Exploiting that needs write access
to ``/dev`` itself, i.e. an attacker already root on the flashing host,
so this is defense-in-depth, not the primary guarantee -- named here so
the guarantee this docstring claims doesn't overstate what's actually
enforced.  An explicit ``flash_args.confirm: true`` (or
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

import errno
import os
import posixpath
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


def _to_posix(value: str) -> str:
    """Render ``value`` as a POSIX-style (``/``-separated) path string.

    Device paths (``/dev/sdb``) are always POSIX text, regardless of
    the host this validation runs on -- a Linux ``/dev`` node is
    meaningless reinterpreted through ``\\``-separated NT path
    semantics.  A no-op on POSIX hosts, where ``os.sep`` already is
    ``/``; on Windows, normalizes the host's own separator first so
    the traversal check below sees the same path shape either way.
    """
    return value.replace(os.sep, "/") if os.sep != "/" else value


def _resolve_dev_root(target: str) -> "tuple[Path | None, str | None]":
    """Canonicalize ``target`` and require the result to stay strictly
    beneath ``_DEV_ROOT``.

    Two layers, in order:

    1. Lexical traversal collapse via ``posixpath.normpath`` on a
       POSIX-normalized copy of the string -- catches
       ``/dev/../tmp/foo`` on every host, including Windows, where the
       host's own ``os.path``/``pathlib`` would instead reinterpret the
       string as an NT path (turning ``/dev/sdb`` into ``\\dev\\sdb``
       both in the "not beneath /dev/" error text and, far worse, in
       the argv actually handed to ``dd``/``bmaptool``).
    2. Real filesystem resolution (``os.path.realpath``), which
       additionally chases symlinks -- run only when the target
       actually EXISTS on this host (``os.path.lexists``), on any OS.

       Existence, not ``os.name``, is the discriminator: ``realpath`` of
       a path that isn't there tells us nothing and actively harms, because
       on Windows it drive-anchors the result (``/dev/sdb`` becomes
       ``D:\\dev\\sdb``) which would then fail the beneath-root check and
       reject a legitimate target -- including the eMMC ``--dry-run``
       preview, whose whole point is that the device need not be plugged in
       yet.  Conversely, gating on ``os.name == "posix"`` failed OPEN: a
       symlink escape inside a real, existing tree went unresolved on
       Windows.  ``lexists`` rather than ``exists`` so a dangling symlink is
       still followed instead of being treated as absent.

    Fails closed against traversal and symlink escapes.  Does not
    require the target to exist -- safe to run unconditionally,
    including for a dry-run preview of a target that isn't plugged in
    yet.  The existence + block-device check is a separate, stricter
    gate (see ``_require_block_device``) applied only right before a
    tool is actually about to be invoked.

    Returns ``(resolved_path, None)`` on success or ``(None, message)``
    on rejection.  The returned path is for validation/stat use only
    (``_require_block_device``) -- callers must keep using the
    *original* ``target`` string, not this return value, when building
    a command argv.
    """
    root = _to_posix(_DEV_ROOT).rstrip("/") or "/"
    normalized = posixpath.normpath(_to_posix(target))
    # Chase symlinks only when the path actually EXISTS on this host --
    # not when `os.name == "posix"`.  Keying on the OS was too coarse in
    # both directions: on Windows it skipped symlink resolution even for
    # a tree that genuinely exists (so a symlink escape went undetected,
    # which is failing OPEN), while on any host `os.path.realpath` of a
    # NON-existent `/dev/sdb` drive-anchors it (`D:\dev\sdb` on Windows),
    # which would then fail the beneath-root check and reject a perfectly
    # legitimate target -- breaking the eMMC `--dry-run` preview whose
    # whole point is that the device need not be plugged in yet.
    #
    # Existence is the property that actually decides whether realpath
    # can tell us anything, and it is host-independent.  `lexists` (not
    # `exists`) so a symlink pointing at a missing target is still
    # followed rather than silently treated as absent.
    if os.path.lexists(target):
        resolved_str = os.path.realpath(target)
    else:
        resolved_str = normalized
    resolved_posix = _to_posix(resolved_str)

    if resolved_posix != root and not resolved_posix.startswith(root + "/"):
        return None, (
            f"yocto_wic: refusing target '{target}' -- resolves to "
            f"'{resolved_str}', which is not beneath {root}/.  Set "
            f"flash_args.target to a real block device under {root}/."
        )
    if resolved_posix == root:
        return None, (
            f"yocto_wic: refusing target '{target}' -- resolves to "
            f"{root} itself, not a device beneath it."
        )
    return Path(resolved_str), None


def _require_block_device(resolved: Path) -> "str | None":
    """Require ``resolved`` (already root-checked by
    ``_resolve_dev_root``) to ``stat`` as a real block device.

    Regular files are never a valid flash target for this backend --
    there is no silent fallback.  Called only when a tool is actually
    about to be invoked (not during a dry-run/planning preview), so it
    never blocks previewing a target that isn't physically present yet.

    Not atomic with the ``dd``/``bmaptool`` invocation that follows:
    this ``stat``s ``resolved`` by name, and the tool re-opens that same
    name afterward -- a target swapped out from under the process in
    between (TOCTOU) would slip past this check.  Exploiting that
    requires write access to ``/dev`` itself, i.e. an attacker already
    root on the flashing host, so this is defense-in-depth rather than
    the primary guarantee.

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


_PROC_MOUNTINFO = Path("/proc/self/mountinfo")
_PROC_SWAPS = Path("/proc/swaps")
_SYS_BLOCK = Path("/sys/class/block")
_SYS_DEV_BLOCK = Path("/sys/dev/block")


def _block_family(name: str, seen: "set[str] | None" = None) -> "set[str]":
    """Return ``name`` plus every whole disk beneath it (partition parent,
    device-mapper / md slaves), using sysfs.
    """
    seen = seen if seen is not None else set()
    if name in seen:
        return seen
    seen.add(name)
    node = _SYS_BLOCK / name
    try:
        if (node / "partition").exists():
            _block_family(os.path.basename(os.path.dirname(os.path.realpath(node))), seen)
        if (node / "slaves").is_dir():
            for slave in os.listdir(node / "slaves"):
                _block_family(slave, seen)
    except OSError:
        pass
    return seen


def _busy_block_devices() -> "list[tuple[str, str]]":
    """``(what, kernel block name)`` for every mounted filesystem and active
    swap area.  Mounts are identified by the ``maj:min`` in
    /proc/self/mountinfo, resolved through /sys/dev/block, so a root shown
    as ``/dev/root`` still maps to its real disk.  Raises ``OSError`` when
    the mount table or sysfs cannot be read.
    """
    if not _SYS_DEV_BLOCK.is_dir():
        raise OSError(f"{_SYS_DEV_BLOCK} not available")
    busy: "list[tuple[str, str]]" = []
    for ln in _PROC_MOUNTINFO.read_text(encoding="utf-8").splitlines():
        fields = ln.split()
        if len(fields) < 5:
            continue
        majmin, mountpoint = fields[2], fields[4]
        dev = _SYS_DEV_BLOCK / majmin
        if dev.exists():
            busy.append((f"mounted at {mountpoint}", os.path.basename(os.path.realpath(dev))))
            continue
        # Anonymous device numbers (btrfs, overlay, tmpfs): fall back to the
        # mount source after the " - " separator when it is a /dev node.
        if " - " in ln:
            tail = ln.split(" - ", 1)[1].split()
            if len(tail) >= 2 and tail[1].startswith("/dev/"):
                busy.append((f"mounted at {mountpoint}", os.path.basename(os.path.realpath(tail[1]))))
    # An unreadable /proc/swaps raises: swap cannot be ruled out, so refuse.
    for ln in _PROC_SWAPS.read_text(encoding="utf-8").splitlines()[1:]:
        src = ln.split()[0] if ln.strip() else ""
        if src.startswith("/dev/"):
            busy.append(("active swap", os.path.basename(os.path.realpath(src))))
    return busy


def _rdev(path: Path) -> int:
    """Device number of the node at ``path`` (injectable for tests)."""
    return os.stat(path).st_rdev


def _open_excl(path: str) -> None:
    """Open ``path`` read-only with ``O_EXCL`` and close it.  On a block
    device ``O_EXCL`` without ``O_CREAT`` fails with ``EBUSY`` while the
    kernel holds the device (mounted, swap, dm/md/LUKS member), whichever
    mount namespace the caller lives in.  Injectable for tests.
    """
    os.close(os.open(path, os.O_RDONLY | os.O_EXCL))


def _kernel_name(resolved: Path) -> str:
    """Kernel block name of the node at ``resolved``, by device number
    (``st_rdev`` -> /sys/dev/block/maj:min), so mknod aliases and real-node
    ``/dev/root`` match.  Raises ``OSError`` when it cannot be resolved.
    """
    rdev = _rdev(resolved)
    link = _SYS_DEV_BLOCK / f"{os.major(rdev)}:{os.minor(rdev)}"
    if not link.exists():
        raise OSError(f"no block device {os.major(rdev)}:{os.minor(rdev)} in {_SYS_DEV_BLOCK}")
    return os.path.basename(os.path.realpath(link))


def _kernel_busy(resolved: Path, name: str) -> "str | None":
    """Namespace-independent in-use check on ``name`` and its partitions:
    ``O_EXCL`` open (EBUSY) and a non-empty sysfs ``holders`` (opened LUKS,
    assembled md, an LV in use).  Returns a reason, or raises ``OSError``
    for an errno meaning "cannot tell" (EACCES, EIO, ...), which the caller
    refuses.  ENOENT on a *partition* node is skipped -- a container may
    pass only the whole disk through, and that partition's ``holders`` is
    still checked -- but ENOENT on the target itself is "cannot tell".
    """
    node = _SYS_BLOCK / name
    children = [c for c in sorted(os.listdir(node))
                if (node / c / "partition").exists()]
    for dev in [name, *children]:
        holders = _SYS_BLOCK / dev / "holders"
        if holders.is_dir() and os.listdir(holders):
            return f"'{dev}' has holders (dm/md/LUKS/LV)"
        path = str(resolved) if dev == name else f"/dev/{dev}"
        try:
            _open_excl(path)
        except OSError as exc:
            if exc.errno == errno.EBUSY:
                return f"'{dev}' is held by the kernel (EBUSY)"
            if exc.errno == errno.ENOENT and dev != name:
                continue
            raise
    return None


def _require_unused_device(resolved: Path) -> "str | None":
    """Refuse a target that is mounted, used as swap, or is (part of) the
    disk backing a mounted filesystem -- notably the host's root disk.

    ``confirm`` alone is not a barrier: provision_som.py sets it under
    --execute, so a mistyped ``--emmc-device /dev/sda`` would otherwise
    overwrite the workstation's system disk.  Fails closed: when the mount
    table or sysfs cannot be read the target cannot be proven unused, so it
    is refused, as is an unreadable /proc/swaps.  Also namespace-independent
    (container with the host disk passed through): ``O_EXCL`` EBUSY and sysfs
    holders.  Multi-device btrfs members other than the one the kernel
    reports, and ZFS pool members, are not detected.  Returns ``None`` when
    safe.
    """
    try:
        busy = _busy_block_devices()
    except OSError as exc:
        return (
            f"yocto_wic: refusing target '{resolved}' -- cannot verify it is "
            f"not mounted ({exc}); write from a Linux host with /proc and /sys."
        )
    try:
        target = _kernel_name(resolved)
        reason = _kernel_busy(resolved, target)
    except OSError as exc:
        return (
            f"yocto_wic: refusing target '{resolved}' -- cannot verify it is "
            f"idle ({exc})."
        )
    if reason:
        return (
            f"yocto_wic: refusing target '{resolved}' -- {reason}; it is in "
            f"use on this host (any namespace); release it first."
        )
    for what, name in busy:
        if target in _block_family(name):
            return (
                f"yocto_wic: refusing target '{resolved}' -- it is or backs a "
                f"device in use ('{name}', {what}); unmount it (and never "
                f"target the host's root disk)."
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
                encoding="utf-8",
                errors="replace",
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
        target = str((ctx.flash_args or {}).get("target") or "")
        if not target:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("yocto_wic: flash_args.target is required "
                         "(e.g. /dev/sdb)"),
            )
        resolved_target, target_err = _resolve_dev_root(target)
        if target_err:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=target_err,
            )
        # `target` deliberately stays the original (already-validated)
        # POSIX device-path string, never the resolved form: on a
        # non-POSIX host `resolved_target` is only lexically normalized
        # (see `_resolve_dev_root`), and even on POSIX a symlink target
        # like `/dev/by-id/mmc-foo` should reach `dd`/`bmaptool` as the
        # name the caller asked for, not the realpath'd device node.
        # `resolved_target` is used only for the block-device stat check
        # below.

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

        busy_err = _require_unused_device(resolved_target)
        if busy_err:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=busy_err,
            )

        if pipeline is not None:
            proc = _run_pipeline(*pipeline)
        else:
            proc = subprocess.run(cmd, check=False,
                                  capture_output=True, text=True, encoding="utf-8", errors="replace",
                                  env={**os.environ, "PYTHONIOENCODING": "utf-8"})
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
