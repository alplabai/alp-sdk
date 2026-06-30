"""`alp faultdecode` -- decode an ARM Cortex-M (ARMv8-M) fault dump.

A firmware engineer pastes the fault registers a HardFault prints (CFSR, HFSR,
optionally DFSR, BFAR, MMFAR) and gets back the human-readable cause plus, when
an ELF is supplied, the faulting symbol and `file:line` -- instead of staring at
CFSR hex.

Targets the ARMv8-M fault model shared by the SoMs this SDK drives: Cortex-M55
(ARMv8.1-M, AEN ``m55_hp`` / ``m55_he``) and Cortex-M33 (ARMv8-M, V2N CM33).

It is strictly HW-free: pure register arithmetic.  Symbolication is best-effort
and optional -- if no ELF or no ``addr2line``-class tool is found it is skipped,
never fatal.  Exit code is 0 for any successful analysis (even "no fault flags
set"); only genuinely bad input (no registers, or an unparseable value) is
nonzero.

The decode core (``decode``) is a pure function: feed it register integers and
it returns a :class:`FaultReport` with zero I/O, so it unit-tests without an ELF
or a board.
"""

from __future__ import annotations

import json as _json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import click

# Reuse the validator's colour policy (honours NO_COLOR / non-tty) rather than
# reinventing it; the Fore/Style fallback mirrors diagnostic.py so the command
# still runs if colorama is somehow absent.
from alp_cli.diagnostic import _use_color

try:
    from colorama import Fore, Style
except ImportError:  # pragma: no cover - colorama is a hard dependency
    class _Stub:
        def __getattr__(self, _: str) -> str:
            return ""

    Fore = _Stub()  # type: ignore[assignment]
    Style = _Stub()  # type: ignore[assignment]


# -------- bit tables ----------------------------------------------------------
#
# Each entry is (bit, flag-name, one-line plain-English meaning).  Bit numbers
# for MMFSR/BFSR/UFSR are absolute positions inside the 32-bit CFSR word, so the
# same mask logic works whether the caller supplied a combined CFSR or the three
# sub-registers separately.  References: ARMv8-M Architecture Reference Manual,
# SCB CFSR/HFSR/DFSR.

# MemManage Fault Status (CFSR bits 0-7).
MMFSR_BITS: tuple[tuple[int, str, str], ...] = (
    (0, "IACCVIOL", "Instruction access violation -- the MPU blocked an instruction fetch "
                    "(executing from a no-execute / unprivileged region)."),
    (1, "DACCVIOL", "Data access violation -- the MPU blocked a load/store; MMFAR holds the address."),
    (3, "MUNSTKERR", "MemManage fault unstacking on exception return -- the exception frame sits "
                     "in MPU-protected memory (often a corrupted/overflowed stack)."),
    (4, "MSTKERR", "MemManage fault stacking on exception entry -- the stack pointer points into "
                   "MPU-protected/invalid memory (a bad or overflowed stack)."),
    (5, "MLSPERR", "MemManage fault during lazy floating-point state preservation."),
    (7, "MMARVALID", "MMFAR holds a valid faulting data address (see below)."),
)

# BusFault Status (CFSR bits 8-15).
BFSR_BITS: tuple[tuple[int, str, str], ...] = (
    (8, "IBUSERR", "Instruction bus error -- a prefetch faulted, usually a branch/call through a "
                   "bad function pointer into unmapped memory."),
    (9, "PRECISERR", "Precise data bus error -- a load/store faulted and BFAR holds the exact "
                     "faulting address (commonly an unclocked/absent peripheral or a bad pointer)."),
    (10, "IMPRECISERR", "Imprecise data bus error -- a buffered/late write faulted; the PC has moved "
                        "on, so BFAR is not reliable (look for an earlier bad store)."),
    (11, "UNSTKERR", "Bus fault unstacking on exception return -- a corrupted stack pointer."),
    (12, "STKERR", "Bus fault stacking on exception entry -- a bad or overflowed stack pointer."),
    (13, "LSPERR", "Bus fault during lazy floating-point state preservation."),
    (15, "BFARVALID", "BFAR holds a valid faulting data address (see below)."),
)

# UsageFault Status (CFSR bits 16-31).
UFSR_BITS: tuple[tuple[int, str, str], ...] = (
    (16, "UNDEFINSTR", "Undefined instruction -- executed a bad/corrupted opcode (often a wild PC "
                       "or jumping into data)."),
    (17, "INVSTATE", "Invalid state -- EPSR.T cleared or illegal IT state, classically a function "
                     "pointer called without the Thumb bit (bit 0) set."),
    (18, "INVPC", "Invalid PC on exception return -- a bad EXC_RETURN or a corrupted stacked PC."),
    (19, "NOCP", "No coprocessor -- access to a disabled/absent coprocessor, most often the FPU "
                 "used before CPACR enables it."),
    (20, "STKOF", "Stack overflow -- the hardware stack-limit (PSPLIM/MSPLIM) tripped (ARMv8-M)."),
    (24, "UNALIGNED", "Unaligned access -- an unaligned load/store while alignment trapping is on "
                      "(or an LDM/STM/LDRD that must be aligned)."),
    (25, "DIVBYZERO", "Divide by zero -- SDIV/UDIV by zero with DIV_0_TRP enabled."),
)

# HardFault Status (HFSR).
HFSR_BITS: tuple[tuple[int, str, str], ...] = (
    (1, "VECTTBL", "Vector-table read fault -- a bus error reading an exception vector."),
    (30, "FORCED", "Forced HardFault -- a configurable fault (MemManage/BusFault/UsageFault) was "
                   "escalated; the real cause is in CFSR above."),
    (31, "DEBUGEVT", "Debug event -- a breakpoint/watchpoint fired with no debugger attached."),
)

# Debug Fault Status (DFSR) -- optional, informational.
DFSR_BITS: tuple[tuple[int, str, str], ...] = (
    (0, "HALTED", "Halt request (debugger single-step / halt)."),
    (1, "BKPT", "Breakpoint -- a BKPT instruction or hardware breakpoint."),
    (2, "DWTTRAP", "DWT watchpoint / debug-monitor trap."),
    (3, "VCATCH", "Vector catch triggered."),
    (4, "EXTERNAL", "External debug request (EDBGRQ)."),
)


# -------- result types --------------------------------------------------------


@dataclass(slots=True)
class DecodedFlag:
    """One set status-register bit, with its register and plain-English meaning."""

    reg: str  # MMFSR | BFSR | UFSR | HFSR | DFSR
    name: str
    bit: int  # absolute bit position within its register word
    meaning: str


@dataclass(slots=True)
class FaultReport:
    """The pure decode result: set flags, faulting addresses, and a root cause."""

    flags: list[DecodedFlag] = field(default_factory=list)
    bfar: int | None = None
    bfar_valid: bool = False
    mmfar: int | None = None
    mmfar_valid: bool = False
    root_cause: str = ""
    inputs: dict[str, int] = field(default_factory=dict)

    @property
    def fault_detected(self) -> bool:
        return bool(self.flags)

    def has(self, name: str) -> bool:
        return any(f.name == name for f in self.flags)


# -------- the pure decode core ------------------------------------------------


def _scan(value: int, table: tuple[tuple[int, str, str], ...], reg: str) -> list[DecodedFlag]:
    return [
        DecodedFlag(reg=reg, name=name, bit=bit, meaning=meaning)
        for bit, name, meaning in table
        if value & (1 << bit)
    ]


def decode(
    *,
    cfsr: int = 0,
    hfsr: int = 0,
    dfsr: int = 0,
    bfar: int | None = None,
    mmfar: int | None = None,
) -> FaultReport:
    """Decode ARMv8-M fault registers into a :class:`FaultReport`.

    Pure function -- no I/O, no shelling out -- so it is trivially unit-testable.
    ``cfsr`` carries MMFSR (bits 0-7), BFSR (bits 8-15) and UFSR (bits 16-31).
    ``bfar``/``mmfar`` are only treated as authoritative when the matching VALID
    bit is set in CFSR; an address supplied without its VALID bit is reported but
    flagged as possibly stale.
    """
    report = FaultReport()
    report.inputs = {
        "cfsr": cfsr,
        "hfsr": hfsr,
        "dfsr": dfsr,
        **({"bfar": bfar} if bfar is not None else {}),
        **({"mmfar": mmfar} if mmfar is not None else {}),
    }

    report.flags.extend(_scan(cfsr, MMFSR_BITS, "MMFSR"))
    report.flags.extend(_scan(cfsr, BFSR_BITS, "BFSR"))
    report.flags.extend(_scan(cfsr, UFSR_BITS, "UFSR"))
    report.flags.extend(_scan(hfsr, HFSR_BITS, "HFSR"))
    report.flags.extend(_scan(dfsr, DFSR_BITS, "DFSR"))

    report.bfar_valid = report.has("BFARVALID")
    report.mmfar_valid = report.has("MMARVALID")
    report.bfar = bfar
    report.mmfar = mmfar

    report.root_cause = _root_cause(report)
    return report


def _addr_phrase(report: FaultReport) -> str:
    if report.bfar_valid and report.bfar is not None:
        return f" at 0x{report.bfar:08x} (BFAR)"
    if report.mmfar_valid and report.mmfar is not None:
        return f" at 0x{report.mmfar:08x} (MMFAR)"
    return ""


def _root_cause(report: FaultReport) -> str:
    """Pick the single most likely root cause from the set flags.

    Ordered most-specific-first: a precise address or a stack-overflow trap tells
    you far more than the generic "forced HardFault" escalation bit, so those win.
    """
    if not report.flags:
        return "No fault status bits are set -- nothing to decode."

    addr = _addr_phrase(report)

    if report.has("STKOF"):
        return ("Stack overflow: the ARMv8-M stack-limit register (PSPLIM/MSPLIM) tripped. "
                "Grow the offending thread/ISR stack or fix unbounded recursion / large stack buffers.")
    if report.has("PRECISERR"):
        return (f"Precise data bus fault{addr or ''} -- a load/store hit a faulting address, "
                "commonly an access to an unclocked/absent peripheral or a bad/dangling pointer.")
    if report.has("IMPRECISERR"):
        return ("Imprecise data bus fault -- a buffered write faulted after the CPU moved on, so the "
                "PC/BFAR do not pinpoint it. Suspect an earlier bad store; a DSB after suspect writes "
                "makes it precise.")
    if report.has("DACCVIOL"):
        return (f"MPU data access violation{addr or ''} -- a load/store hit a region the MPU forbids "
                "(wrong permissions, or an unmapped/unprivileged address).")
    if report.has("IACCVIOL"):
        return ("MPU instruction access violation -- the core tried to execute from a no-execute / "
                "forbidden region (often a corrupted PC or a bad function pointer).")
    if report.has("IBUSERR"):
        return ("Instruction bus fault -- a fetch faulted, typically a branch/call through a bad "
                "function pointer into unmapped memory.")
    if report.has("MSTKERR") or report.has("STKERR"):
        return ("Fault while stacking the exception frame on entry -- the stack pointer is bad or "
                "overflowed (check SP/PSPLIM and the offending stack's size).")
    if report.has("MUNSTKERR") or report.has("UNSTKERR"):
        return ("Fault while unstacking on exception return -- the saved exception frame is corrupted "
                "(a stack overwrite or a clobbered SP).")
    if report.has("DIVBYZERO"):
        return ("Divide by zero -- SDIV/UDIV with a zero divisor and DIV_0_TRP enabled. Guard the "
                "divisor or disable the trap.")
    if report.has("UNALIGNED"):
        return ("Unaligned access fault -- an unaligned load/store with alignment trapping on (or an "
                "LDM/STM/LDRD that requires alignment). Fix the pointer alignment or use packed access.")
    if report.has("INVSTATE"):
        return ("Invalid state (EPSR.T / IT) -- almost always a function pointer called without the "
                "Thumb bit (bit 0) set, or a corrupted PSR.")
    if report.has("INVPC"):
        return ("Invalid PC on exception return -- a bad EXC_RETURN value or a corrupted stacked PC "
                "(stack overflow / FNC return into a clobbered frame).")
    if report.has("NOCP"):
        return ("No-coprocessor fault -- code used a coprocessor that is disabled, most often the FPU "
                "before CPACR (CP10/CP11) enables it. Enable the FPU in CPACR / Kconfig.")
    if report.has("UNDEFINSTR"):
        return ("Undefined instruction -- a corrupted/wild PC executed a bad opcode, or code was "
                "built for a different ISA than the running core.")
    if report.has("VECTTBL"):
        return ("Vector-table read fault -- a bus error reading an exception vector (VTOR points at "
                "bad memory, or the vector table is unmapped).")
    if report.has("DEBUGEVT"):
        return ("Debug event with no debugger attached -- a stray BKPT or a watchpoint firing in a "
                "free-running build.")
    if report.has("FORCED"):
        return ("Forced HardFault -- a configurable fault escalated but its own status bits are clear; "
                "the escalation usually means faults are disabled (SHCSR) or it faulted at priority -1.")

    first = report.flags[0]
    return f"{first.name} set ({first.reg}): {first.meaning}"


# -------- dump parsing --------------------------------------------------------
#
# Register tokens we will grep out of a pasted dump.  Maps the token (matched
# case-insensitively) to a canonical key.  Sub-registers (mmfsr/bfsr/ufsr) get
# composed back into CFSR.  Order matters only for the regex alternation; the
# longer tokens are listed first so e.g. "mmfar" is not eaten by a shorter name.

_DUMP_TOKENS: tuple[tuple[str, str], ...] = (
    ("cfsr", "cfsr"),
    ("hfsr", "hfsr"),
    ("dfsr", "dfsr"),
    ("mmfar", "mmfar"),
    ("bfar", "bfar"),
    ("mmfsr", "mmfsr"),
    ("bfsr", "bfsr"),
    ("ufsr", "ufsr"),
    ("pc", "pc"),
    ("lr", "lr"),
)

# token, optional same-line noise (e.g. "Address:"), then a 0x or bare-hex value.
# The noise gap allows any non-newline chars (non-greedy) so hex-letter words
# like "Address" between the name and its value are skipped; the value still has
# to be a 0x literal or a word-bounded hex run, so it cannot land mid-word.
_DUMP_RE = re.compile(
    r"(?i)\b(" + "|".join(re.escape(t) for t, _ in _DUMP_TOKENS) + r")\b"
    r"[^\r\n]{0,24}?"
    r"(0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{2,8}\b)"
)


def parse_dump(text: str) -> dict[str, int]:
    """Grep known register names + values out of a pasted fault dump.

    Recognises ``CFSR``/``HFSR``/``DFSR``/``BFAR``/``MMFAR`` plus the split
    ``MMFSR``/``BFSR``/``UFSR`` (composed back into CFSR) and ``PC``/``LR``.
    Accepts ``NAME: 0x..`` / ``NAME = 0x..`` / ``MMFAR Address: 0x..`` shapes and
    bare hex.  Last occurrence of a token wins.
    """
    canon = dict(_DUMP_TOKENS)
    found: dict[str, int] = {}
    for m in _DUMP_RE.finditer(text):
        key = canon[m.group(1).lower()]
        raw = m.group(2)
        try:
            found[key] = int(raw, 16)
        except ValueError:  # pragma: no cover - regex already constrains this
            continue

    # Compose CFSR from sub-registers if a combined CFSR was not given outright.
    if "cfsr" not in found:
        composed = 0
        if "mmfsr" in found:
            composed |= found["mmfsr"] & 0xFF
        if "bfsr" in found:
            composed |= (found["bfsr"] & 0xFF) << 8
        if "ufsr" in found:
            composed |= (found["ufsr"] & 0xFFFF) << 16
        if composed:
            found["cfsr"] = composed
    for k in ("mmfsr", "bfsr", "ufsr"):
        found.pop(k, None)
    return found


# -------- symbolication (best-effort, optional) -------------------------------

_ADDR2LINE_TOOLS = ("arm-zephyr-eabi-addr2line", "llvm-addr2line", "addr2line")


@dataclass(slots=True)
class Symbol:
    addr: int
    func: str
    location: str  # file:line, or "??:?" when unknown


def resolve_symbol(addr: int, elf: Path) -> Symbol | None:
    """Resolve ``addr`` to ``func`` + ``file:line`` via an addr2line-class tool.

    Tries ``arm-zephyr-eabi-addr2line`` then ``llvm-addr2line`` then plain
    ``addr2line``.  Returns ``None`` (caller skips gracefully) if no tool is on
    PATH or the lookup fails -- symbolication is a convenience, never required.
    """
    tool = next((t for t in _ADDR2LINE_TOOLS if shutil.which(t)), None)
    if tool is None or not elf.is_file():
        return None
    try:
        proc = subprocess.run(
            [tool, "-f", "-C", "-e", str(elf), f"0x{addr:x}"],
            capture_output=True, text=True, timeout=15, check=False,
        )
    except (OSError, subprocess.SubprocessError):  # pragma: no cover - env-dependent
        return None
    lines = [ln.strip() for ln in (proc.stdout or "").splitlines() if ln.strip()]
    if not lines:
        return None
    func = lines[0]
    location = lines[1] if len(lines) > 1 else "??:?"
    if func in ("??", "") and location in ("??:?", "??:0", ":?"):
        return None  # tool ran but knows nothing about this address
    return Symbol(addr=addr, func=func or "??", location=location or "??:?")


# -------- rendering -----------------------------------------------------------

_REG_HUE = {
    "MMFSR": Fore.MAGENTA,
    "BFSR": Fore.RED,
    "UFSR": Fore.YELLOW,
    "HFSR": Fore.RED,
    "DFSR": Fore.CYAN,
}


def _paint(s: str, hue: str, color: bool) -> str:
    return f"{hue}{s}{Style.RESET_ALL}" if color else s


def render_human(
    report: FaultReport,
    symbols: dict[str, Symbol] | None,
    color: bool,
) -> str:
    lines: list[str] = []
    head = "ARM Cortex-M (ARMv8-M) fault decode"
    lines.append(_paint(head, Fore.CYAN + Style.BRIGHT, color))

    # Echo the registers we actually decoded.
    reg_bits = [f"{k.upper()}=0x{v:08x}" for k, v in report.inputs.items()]
    lines.append("  " + "  ".join(reg_bits))
    lines.append("")

    if not report.fault_detected:
        lines.append(_paint("  No fault flags set.", Fore.GREEN, color))
        lines.append("  " + report.root_cause)
        return "\n".join(lines)

    lines.append(_paint("Set flags:", Fore.WHITE + Style.BRIGHT, color))
    for f in report.flags:
        tag = _paint(f"[{f.reg}] {f.name}", _REG_HUE.get(f.reg, "") , color)
        lines.append(f"  {tag} (bit {f.bit}): {f.meaning}")

    # Faulting addresses.
    if report.bfar is not None:
        note = "" if report.bfar_valid else "  (BFARVALID not set -- address may be stale)"
        lines.append(f"  Faulting address (BFAR): 0x{report.bfar:08x}{note}")
    if report.mmfar is not None:
        note = "" if report.mmfar_valid else "  (MMARVALID not set -- address may be stale)"
        lines.append(f"  Faulting address (MMFAR): 0x{report.mmfar:08x}{note}")

    lines.append("")
    lines.append(_paint("Most likely cause:", Fore.WHITE + Style.BRIGHT, color))
    lines.append("  " + report.root_cause)

    if symbols:
        lines.append("")
        lines.append(_paint("Symbolication:", Fore.WHITE + Style.BRIGHT, color))
        for which, sym in symbols.items():
            lines.append(
                f"  {which.upper()} 0x{sym.addr:08x} -> {_paint(sym.func, Fore.GREEN, color)} "
                f"({sym.location})"
            )

    return "\n".join(lines)


def report_to_json(report: FaultReport, symbols: dict[str, Symbol] | None) -> dict:
    """Machine-readable shape for the extension's troubleshooting panel."""
    return {
        "fault_detected": report.fault_detected,
        "inputs": {k: f"0x{v:08x}" for k, v in report.inputs.items()},
        "flags": [
            {"reg": f.reg, "name": f.name, "bit": f.bit, "meaning": f.meaning}
            for f in report.flags
        ],
        "addresses": {
            "bfar": None if report.bfar is None else f"0x{report.bfar:08x}",
            "bfar_valid": report.bfar_valid,
            "mmfar": None if report.mmfar is None else f"0x{report.mmfar:08x}",
            "mmfar_valid": report.mmfar_valid,
        },
        "root_cause": report.root_cause,
        "symbols": (
            None
            if not symbols
            else {
                which: {"addr": f"0x{s.addr:08x}", "func": s.func, "location": s.location}
                for which, s in symbols.items()
            }
        ),
    }


# -------- CLI plumbing --------------------------------------------------------


class _HexInt(click.ParamType):
    """A click param type that parses fault-register values, hex by default.

    These are CPU status registers, which are always read in hex, so a bare
    ``8200`` means ``0x8200`` (not decimal 8200) and ``0x8200`` works too.  Bad
    input raises a BadParameter (the tool's only nonzero exit path).
    """

    name = "hexint"

    def convert(self, value, param, ctx):  # type: ignore[override]
        if isinstance(value, int):
            return value
        text = str(value).strip()
        try:
            # base 16 accepts an optional 0x prefix and a bare hex run alike.
            return int(text, 16)
        except ValueError:
            self.fail(f"{value!r} is not a valid integer (try 0x...)", param, ctx)


HEXINT = _HexInt()


@click.command(name="faultdecode",
               help="Decode an ARM Cortex-M (ARMv8-M, M33/M55) fault dump.")
@click.option("--cfsr", type=HEXINT, default=None, help="Configurable Fault Status Register.")
@click.option("--hfsr", type=HEXINT, default=None, help="HardFault Status Register.")
@click.option("--dfsr", type=HEXINT, default=None, help="Debug Fault Status Register (optional).")
@click.option("--bfar", type=HEXINT, default=None, help="BusFault Address Register.")
@click.option("--mmfar", type=HEXINT, default=None, help="MemManage Fault Address Register.")
@click.option("--mmfsr", type=HEXINT, default=None, help="MemManage sub-register (composed into CFSR).")
@click.option("--bfsr", type=HEXINT, default=None, help="BusFault sub-register (composed into CFSR).")
@click.option("--ufsr", type=HEXINT, default=None, help="UsageFault sub-register (composed into CFSR).")
@click.option("--pc", type=HEXINT, default=None, help="Program Counter (symbolicated with --elf).")
@click.option("--lr", type=HEXINT, default=None, help="Link Register (symbolicated with --elf).")
@click.option("--elf", type=click.Path(exists=True, dir_okay=False, path_type=Path), default=None,
              help="ELF for --pc/--lr symbolication (best-effort; skipped if no tool/elf).")
@click.option("--file", "file_",
              type=click.Path(exists=True, dir_okay=False, allow_dash=True, path_type=Path),
              default=None, help="Read a pasted dump from this file ('-' for stdin) and grep registers.")
@click.option("--json", "as_json", is_flag=True, help="Emit a machine-readable JSON report.")
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
def faultdecode_cmd(
    cfsr, hfsr, dfsr, bfar, mmfar, mmfsr, bfsr, ufsr, pc, lr, elf, file_, as_json, no_color,
) -> None:
    """Decode ARMv8-M fault registers into a human cause + (optionally) a symbol.

    Supply registers as flags, and/or paste a dump via ``--file`` / stdin and it
    greps the register names out.  Explicit flags win over a parsed dump.
    """
    # 1. Gather registers: start from a parsed dump, then let explicit flags win.
    parsed: dict[str, int] = {}
    dump_text = _read_dump(file_)
    if dump_text:
        parsed = parse_dump(dump_text)

    def pick(name: str, flag_val):
        return flag_val if flag_val is not None else parsed.get(name)

    cfsr_v = pick("cfsr", cfsr)
    # Compose CFSR from explicit sub-registers when no combined CFSR was given.
    if cfsr is None and (mmfsr is not None or bfsr is not None or ufsr is not None):
        cfsr_v = ((mmfsr or 0) & 0xFF) | (((bfsr or 0) & 0xFF) << 8) | (((ufsr or 0) & 0xFFFF) << 16)

    hfsr_v = pick("hfsr", hfsr)
    dfsr_v = pick("dfsr", dfsr)
    bfar_v = pick("bfar", bfar)
    mmfar_v = pick("mmfar", mmfar)
    pc_v = pick("pc", pc)
    lr_v = pick("lr", lr)

    # No status registers at all => bad input (this is an analysis tool, but it
    # needs *something* to analyse). Exit nonzero with a usage hint.
    if cfsr_v is None and hfsr_v is None and dfsr_v is None:
        raise click.UsageError(
            "no fault registers supplied -- pass --cfsr/--hfsr/--dfsr "
            "or pipe a dump via --file/-/stdin."
        )

    report = decode(
        cfsr=cfsr_v or 0,
        hfsr=hfsr_v or 0,
        dfsr=dfsr_v or 0,
        bfar=bfar_v,
        mmfar=mmfar_v,
    )

    # 2. Best-effort symbolication (optional, never fatal).
    symbols: dict[str, Symbol] = {}
    if elf is not None:
        for which, addr in (("pc", pc_v), ("lr", lr_v)):
            if addr is not None:
                sym = resolve_symbol(addr, elf)
                if sym is not None:
                    symbols[which] = sym

    if as_json:
        click.echo(_json.dumps(report_to_json(report, symbols or None), indent=2))
    else:
        color = _use_color(False if no_color else None)
        click.echo(render_human(report, symbols or None, color))
        if pc_v is not None and elf is None:
            click.echo(
                "  (note: --pc given without --elf -- pass --elf <app.elf> to resolve the symbol)",
                err=True,
            )

    # Analysis tool: a successful decode is always exit 0 (even "no flags set").


def _read_dump(file_: Path | None) -> str:
    """Read a pasted dump from --file, '-' (stdin), or piped stdin."""
    if file_ is not None:
        if str(file_) == "-":
            return sys.stdin.read()
        return file_.read_text(encoding="utf-8", errors="ignore")
    # Auto-consume piped stdin (non-tty) so `... | alp faultdecode` just works.
    if not sys.stdin.isatty():
        try:
            return sys.stdin.read()
        except (OSError, ValueError):  # pragma: no cover - env-dependent
            return ""
    return ""
