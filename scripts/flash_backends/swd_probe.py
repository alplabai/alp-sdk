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
                     Must match ^[a-z0-9][a-z0-9_-]*$ -- it becomes a
                     `-f interface/{value}.cfg` argument that OpenOCD
                     sources as Tcl, so a path-shaped value is rejected
                     rather than interpolated.
  target       str   OpenOCD target ID ("gd32g553"). Required for the
                     openocd/pyocd path; ignored by J-Link. Same
                     ^[a-z0-9][a-z0-9_-]*$ constraint as `interface`, for
                     the same `-f target/{value}.cfg` reason.
  base         str?  Flash base address (default "0x08000000"). Parsed as a
                     bounded decimal or 0x-prefixed hex integer literal and
                     re-rendered canonically -- the raw string is never
                     interpolated into a generated script.
  reset        bool  Reset+run after programming (default True).
  use_pyocd    bool  Force the pyocd path (skip J-Link + openocd).
  use_openocd  bool  Force the openocd path (skip J-Link auto-pick).
  jlink_device str?  SEGGER device name (default "GD32G553MEY7TR"), passed
                     to `JLinkExe -device` -- this is the PROGRAMMING device
                     for an actual write.  NOTE: `jlink_device` is
                     backend-local, not a single global key.
                     `scripts/flash_backends/zephyr_west_flash.py` emits a
                     DIFFERENT `flash_args.jlink_device` for a DIFFERENT
                     helper entry: a read-only attach profile (e.g.
                     "Cortex-M55") used only for an `expect_dpidr` preflight
                     read, explicitly not flash-capable.  No collision today
                     -- no single manifest entry is read by both backends --
                     but don't assume one backend's `jlink_device` describes
                     what the other would do with it.
  jlink_speed  int?  SWD speed in kHz (default 4000).
"""

from __future__ import annotations

import os
import re
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

# GD32G553's 32-bit address space -- the ceiling a "bounded integer" address
# grammar bounds against.  Not a generic 64-bit escape hatch: this backend
# only ever targets this one 32-bit Cortex-M part family.
_MAX_ADDRESS = 0xFFFFFFFF
_ADDRESS_RE = re.compile(r"^(0[xX][0-9a-fA-F]{1,8}|[0-9]{1,10})$")

# `interface`/`target` become `-f <dir>/{value}.cfg` on the OpenOCD command
# line, and OpenOCD *sources* a `-f` file as Tcl -- a traversal payload here
# (e.g. "../../../../tmp/pwn") reaches arbitrary code execution, not just an
# unexpected file read.  Both are config-file basenames, never paths: no
# "/", no leading ".", no whitespace.  Fail closed -- reject, don't sanitize.
_CONFIG_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")

# Control chars (incl. newline/CR, which is what turns one J-Link Commander
# line into two commands) plus DEL -- refused in any string that ends up
# inside a generated tool script.
_CONTROL_CHARS = frozenset(chr(c) for c in range(0x20)) | {chr(0x7F)}


def _parse_address(raw: object) -> "tuple[int | None, str | None]":
    """Parse ``raw`` as a bounded integer address literal.

    Fail closed: only a plain decimal or ``0x``-prefixed hex token is
    accepted -- no separators, no whitespace, no control characters can
    ride along.  Returns ``(value, None)`` on success or
    ``(None, message)`` on rejection.  Callers must render the returned
    int back to text (``_format_address``) rather than ever passing
    ``raw`` through to a generated script.
    """
    text = "" if raw is None else str(raw)
    if not _ADDRESS_RE.match(text):
        return None, (
            f"swd_probe: flash_args.base '{raw}' is not a valid address "
            f"literal (expected a plain decimal or 0x-prefixed hex "
            f"integer, e.g. 0x08000000)."
        )
    # Explicit base per branch, not int(text, 0) -- base-0 auto-detect
    # raises ValueError on an ambiguous leading-zero decimal like "0123"
    # (looks octal-ish to it), which would surface as an unhandled
    # exception instead of a clean rejection.  A leading-zero decimal is
    # unambiguous in base 10, so it's accepted.
    value = int(text, 16) if text[:2] in ("0x", "0X") else int(text, 10)
    if not (0 <= value <= _MAX_ADDRESS):
        return None, (
            f"swd_probe: flash_args.base '{raw}' is out of range "
            f"(0..0x{_MAX_ADDRESS:X})."
        )
    return value, None


def _format_address(value: int) -> str:
    """Canonical rendering of a validated address -- always this shape,
    regardless of how the caller spelled it."""
    return f"0x{value:08X}"


def _reject_control_chars(value: str, *, what: str) -> "str | None":
    """Return an error message if ``value`` carries a newline, other
    control character, or is empty; ``None`` if it's safe to interpolate
    (still needs per-tool quoting by the caller -- this only rules out
    the characters no quoting style can make safe)."""
    if not value:
        return f"swd_probe: {what} must not be empty."
    if any(ch in _CONTROL_CHARS for ch in value):
        return (
            f"swd_probe: {what} {value!r} contains a control character "
            f"or newline -- refusing to build a flash script from it."
        )
    return None


def _jlink_quote_path(path: str) -> "str | None":
    """Quote ``path`` as a single J-Link Commander token.

    J-Link Commander has no escape sequence for a double quote embedded
    inside a quoted token, so a path containing one is rejected outright
    (fail closed) rather than guessed at.
    """
    if '"' in path:
        return None
    return f'"{path}"'


def _tcl_quote(value: str) -> "str | None":
    """Brace-quote ``value`` as a single Tcl word for an OpenOCD ``-c``
    command string.

    Tcl's ``{...}`` grouping performs no substitution on its contents
    (no ``$``, ``[...]``, or ``;`` interpretation), but requires
    balanced braces.  Reject any embedded brace outright (fail closed)
    rather than trying to escape it.  A trailing backslash is rejected
    too: inside a brace-quoted word Tcl still honors ``\\}`` as an
    escaped (non-closing) brace, so ``{value\\}`` is one unbalanced word
    -- not injectable (the embedded ``{``/``}`` check already blocks
    anything that could re-close it), but it would surface as a
    confusing "missing close-brace" parse error instead of this
    function's clean rejection.
    """
    if "{" in value or "}" in value or value.endswith("\\"):
        return None
    return "{" + value + "}"


def _validate_config_name(value: str, *, what: str) -> "str | None":
    """Reject ``value`` unless it is a bare OpenOCD config-name token.

    Returns ``None`` when safe to interpolate into ``-f <dir>/{value}.cfg``;
    an error message otherwise.  See ``_CONFIG_NAME_RE`` for why this is a
    fail-closed reject, not a sanitize-and-continue.
    """
    if not _CONFIG_NAME_RE.match(value):
        return (
            f"swd_probe: flash_args.{what} {value!r} is not a valid "
            f"config name (expected to match "
            f"{_CONFIG_NAME_RE.pattern!r}) -- refusing to build an "
            f"OpenOCD -f argument from it."
        )
    return None


def _jlink_commander_script(
    artefact: Path, base: str, do_reset: bool
) -> "tuple[str | None, str | None]":
    """Build the J-Link Commander script that programs ``artefact``.

    ``.bin`` images need an explicit load address; ``.hex`` / ``.elf``
    carry their own addresses so ``loadfile`` suffices.  ``base`` must
    already be a canonical address string (see ``_format_address``) --
    this function does not re-validate it.  Returns
    ``(script, None)`` on success or ``(None, message)`` on rejection.
    """
    path = str(artefact)
    err = _reject_control_chars(path, what="artefact path")
    if err:
        return None, err
    quoted_path = _jlink_quote_path(path)
    if quoted_path is None:
        return None, (
            f"swd_probe: artefact path {path!r} contains a double quote "
            f"-- refusing to build a J-Link script from it."
        )
    lines = ["r", "halt"]
    if artefact.suffix.lower() == ".bin":
        lines.append(f"loadbin {quoted_path}, {base}")
    else:
        lines.append(f"loadfile {quoted_path}")
    if do_reset:
        lines += ["r", "g"]
    lines.append("qc")
    return "\n".join(lines) + "\n", None


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
        base_value, base_err = _parse_address(fa.get("base") or _DEFAULT_BASE)
        if base_err:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start, message=base_err)
        base = _format_address(base_value)
        do_reset = bool(fa.get("reset", True))
        force_pyocd = bool(fa.get("use_pyocd"))
        force_openocd = bool(fa.get("use_openocd"))

        # ---- J-Link (primary; best GD32G5 flash support) ----
        jlink = None if (force_pyocd or force_openocd) else _which_jlink()
        if jlink:
            device = fa.get("jlink_device") or _DEFAULT_JLINK_DEVICE
            speed = str(fa.get("jlink_speed") or _DEFAULT_JLINK_SPEED)
            script, script_err = _jlink_commander_script(
                ctx.artefact_path, base, do_reset)
            if script_err:
                return FlashResult(
                    ok=False, elapsed_s=time.monotonic() - start,
                    message=script_err)
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
        interface = str(fa.get("interface") or "")
        target = str(fa.get("target") or "")
        if not interface or not target:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("swd_probe: flash_args.interface and "
                         "flash_args.target are required for the openocd/pyocd "
                         "path (e.g. interface=cmsis-dap, target=gd32g553) -- "
                         "or install SEGGER J-Link for the primary path."))

        interface_err = _validate_config_name(interface, what="interface")
        if interface_err:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=interface_err)
        target_err = _validate_config_name(target, what="target")
        if target_err:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=target_err)

        openocd = None if force_pyocd else shutil.which("openocd")
        pyocd = shutil.which("pyocd")

        if openocd:
            artefact_str = str(ctx.artefact_path)
            artefact_err = _reject_control_chars(
                artefact_str, what="artefact path")
            if artefact_err:
                return FlashResult(
                    ok=False, elapsed_s=time.monotonic() - start,
                    message=artefact_err)
            quoted_artefact = _tcl_quote(artefact_str)
            if quoted_artefact is None:
                return FlashResult(
                    ok=False, elapsed_s=time.monotonic() - start,
                    message=(f"swd_probe: artefact path {artefact_str!r} "
                             f"contains a Tcl brace -- refusing to build "
                             f"an OpenOCD script from it."))
            program_cmd = f"program {quoted_artefact} verify"
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
