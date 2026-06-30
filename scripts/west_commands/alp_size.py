# SPDX-License-Identifier: Apache-2.0
"""
`west alp-size` -- report per-slice flash / RAM footprint against the
SoM's memory budget, BEFORE you flash.

A firmware engineer building a multi-core `board.yaml` project wants to
know, at a glance: does each Zephyr image actually fit the silicon it
targets?  `west alp-build` lays out one `build/<core>-<os>/` slice per
non-`off` core and records them in `build/system-manifest.yaml`; this
command walks that manifest, measures every Zephyr slice's
`zephyr.elf`, and compares FLASH (text+rodata+data) and RAM
(data+bss+noinit) against the FLASH/RAM totals resolved from the SoM's
SoC metadata.

Customer flow:

    west alp-build examples/peripheral-io/hello-world
    west alp-size  examples/peripheral-io/hello-world
    # gate CI -- non-zero exit if any slice blows its budget:
    west alp-size  examples/peripheral-io/hello-world --fail-over-budget

    # machine-readable form for the VS Code extension (like `alp doctor --json`):
    west alp-size  examples/peripheral-io/hello-world --json

Robustness contract:

  * A slice whose `zephyr.elf` (or footprint source) is missing is
    reported as ``not built`` -- never a crash.
  * Yocto / baremetal slices have no Zephyr image -> ``n/a``.
  * When the SoM budget can't be resolved the usage is still shown with
    total ``unknown``; `--fail-over-budget` SKIPS such slices and says so
    (it never guesses a number, and never fails on an unknown budget).

The FLASH/RAM size is taken from (in order): an `arm-zephyr-eabi-size`
/ `llvm-size` / `size` tool if one is found (Berkeley columns:
``text`` + ``data`` = FLASH, ``data`` + ``bss`` = RAM, where binutils
folds .rodata into text and .noinit into bss); else pyelftools section
headers; else the `rom.json` / `ram.json` footprint files Zephyr emits
in the build dir.

This module is import-safe WITHOUT west installed (the west imports are
guarded) so the deterministic helpers below can be unit-tested directly
-- see tests/scripts/test_alp_size.py.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover - surfaced at runtime, not in unit tests
    yaml = None  # type: ignore[assignment]

try:
    from west import log                          # type: ignore[import-not-found]
    from west.commands import WestCommand          # type: ignore[import-not-found]
    _HAVE_WEST = True
except ImportError:  # pragma: no cover - unit tests run without west installed
    _HAVE_WEST = False

    class WestCommand:  # type: ignore[no-redef]
        """Minimal shim so this module imports without west (unit tests)."""

        def __init__(self, *args, **kwargs):  # noqa: D401,ANN002,ANN003
            pass

    log = None  # type: ignore[assignment]

# Reuse the validator's colour discipline (NO_COLOR / isatty aware).  The
# import is guarded so the module still loads where colorama is absent.
try:
    from colorama import Fore, Style
    from colorama import init as _colorama_init

    _colorama_init()
except ImportError:  # pragma: no cover - colorama is a listed dep
    class _Stub:
        def __getattr__(self, _: str) -> str:
            return ""

    Fore = _Stub()  # type: ignore[assignment]
    Style = _Stub()  # type: ignore[assignment]

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _alp_common import find_sdk_root            # noqa: E402


# Candidate size tools, most-specific first.  The Zephyr SDK ships
# `arm-zephyr-eabi-size`; an LLVM toolchain ships `llvm-size`; a host
# binutils install ships `size`.  All speak the same Berkeley columns.
_SIZE_TOOLS = ("arm-zephyr-eabi-size", "llvm-size", "size")

# A slice whose usage is at/above this fraction of its budget is flagged
# WARN even though it still fits -- a pre-flight "you're close" nudge.
_WARN_FRACTION = 0.90


class AlpSizeError(Exception):
    """Raised for a deterministic pre-flight failure (bad/missing
    manifest).  do_run() converts it into log.die().  Per-slice problems
    (missing elf, unknown budget) are NOT errors -- they are reported as
    a row status."""


# ---------------------------------------------------------------------
# Manifest loading (mirrors alp_renode.load_manifest)
# ---------------------------------------------------------------------


def load_manifest(build_root: Path) -> dict:
    """Load ``<build_root>/system-manifest.yaml`` into a dict.

    Raises AlpSizeError when the file is missing or doesn't parse to a
    mapping.
    """
    if yaml is None:  # pragma: no cover - dependency guard
        raise AlpSizeError(
            "PyYAML is required to read system-manifest.yaml "
            "(pip install pyyaml).")
    mpath = Path(build_root) / "system-manifest.yaml"
    if not mpath.is_file():
        raise AlpSizeError(
            f"no system-manifest.yaml at {mpath}; run "
            f"`west alp-build <app>` first.")
    data = yaml.safe_load(mpath.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AlpSizeError(
            f"{mpath} did not parse to a top-level mapping.")
    return data


def slice_build_dir(slice_: dict, build_root: Path) -> Path:
    """Resolve a slice's build directory: the manifest `build_dir` (anchored
    to build_root if relative) else the canonical ``build/<core>-<os>/``."""
    build_root = Path(build_root)
    bd = slice_.get("build_dir")
    if bd:
        p = Path(bd)
        return p if p.is_absolute() else build_root / p
    return build_root / f"{slice_.get('core_id')}-{slice_.get('os')}"


def slice_elf_path(slice_: dict, build_root: Path) -> Path:
    """``<build_dir>/zephyr/zephyr.elf`` for a Zephyr slice."""
    return slice_build_dir(slice_, build_root) / "zephyr" / "zephyr.elf"


# ---------------------------------------------------------------------
# ELF size extraction
# ---------------------------------------------------------------------


def find_size_tool(
    which: Callable[[str], Optional[str]] = shutil.which,
) -> Optional[str]:
    """Return the first available size tool from `_SIZE_TOOLS`, or None."""
    for name in _SIZE_TOOLS:
        exe = which(name)
        if exe:
            return exe
    return None


def parse_berkeley_size(text: str) -> Optional[tuple[int, int]]:
    """Parse Berkeley-format `size` output into ``(flash, ram)`` bytes.

    Berkeley columns are ``text data bss dec hex filename``; binutils
    folds .rodata into ``text`` and .noinit (NOBITS) into ``bss``, so:

        FLASH = text + data      (everything that occupies the image)
        RAM   = data + bss       (everything that occupies RAM at runtime)

    Returns None when no data row is found.
    """
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            text_b, data_b, bss_b = (int(parts[0]), int(parts[1]),
                                     int(parts[2]))
        except ValueError:
            continue  # header row ("text data bss ...") or noise
        return (text_b + data_b, data_b + bss_b)
    return None


def sizes_from_size_tool(
    elf: Path,
    size_bin: str,
    run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> Optional[tuple[int, int]]:
    """Run the size tool on `elf` and parse its Berkeley output.

    `run` is injectable so the parse path is unit-testable without a
    real toolchain.  Returns None when the tool fails or its output
    doesn't parse.
    """
    try:
        proc = run([size_bin, str(elf)], capture_output=True, text=True)
    except (OSError, ValueError):  # pragma: no cover - exec failure
        return None
    if proc.returncode != 0:
        return None
    return parse_berkeley_size(proc.stdout or "")


def sizes_from_pyelftools(elf: Path) -> Optional[tuple[int, int]]:
    """Classify ALLOC sections straight from the ELF section headers.

    FLASH = every allocated section that occupies the load image
    (PROGBITS: .text/.rodata/.data); RAM = every allocated section live
    at runtime (writable or NOBITS: .data/.bss/.noinit).  Mirrors the
    Berkeley model exactly.  Returns None when pyelftools is absent.
    """
    try:
        from elftools.elf.elffile import ELFFile  # type: ignore[import-untyped]
        from elftools.elf.constants import SH_FLAGS  # type: ignore[import-untyped]
    except ImportError:
        return None
    flash = 0
    ram = 0
    with open(elf, "rb") as fh:
        ef = ELFFile(fh)
        for sec in ef.iter_sections():
            flags = sec["sh_flags"]
            if not (flags & SH_FLAGS.SHF_ALLOC):
                continue
            size = sec["sh_size"]
            is_nobits = sec["sh_type"] == "SHT_NOBITS"
            is_write = bool(flags & SH_FLAGS.SHF_WRITE)
            if not is_nobits:
                flash += size          # occupies the flashed image
            if is_nobits or is_write:
                ram += size            # occupies RAM at runtime
    return (flash, ram)


def _footprint_total(path: Path) -> Optional[int]:
    """Total bytes from a Zephyr `rom.json` / `ram.json` footprint file.

    The footprint root is ``{"symbols": {"size": <total>, ...}}``; older
    layouts put the total at the top level.  Returns None when neither
    shape yields an int.
    """
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if isinstance(data, dict):
        root = data.get("symbols")
        if isinstance(root, dict) and isinstance(root.get("size"), int):
            return root["size"]
        if isinstance(data.get("size"), int):
            return data["size"]
    return None


def sizes_from_footprint_json(build_dir: Path) -> Optional[tuple[int, int]]:
    """``(flash, ram)`` from ``<build_dir>/rom.json`` + ``ram.json``.

    Zephyr writes these with `west build -t rom_report/ram_report --json`.
    Returns None unless BOTH parse to a total.
    """
    build_dir = Path(build_dir)
    rom = _footprint_total(build_dir / "rom.json")
    ram = _footprint_total(build_dir / "ram.json")
    if rom is None or ram is None:
        return None
    return (rom, ram)


def extract_sizes(
    elf: Path,
    build_dir: Path,
    size_bin: Optional[str],
    run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> tuple[Optional[tuple[int, int]], Optional[str]]:
    """Resolve ``(flash, ram)`` bytes for one slice via the best source.

    Returns ``((flash, ram), source_label)`` or ``(None, None)`` when no
    source yields a measurement.  Source order: size tool -> pyelftools
    -> rom/ram.json.
    """
    if elf.is_file():
        if size_bin:
            sizes = sizes_from_size_tool(elf, size_bin, run=run)
            if sizes is not None:
                return sizes, "size-tool"
        sizes = sizes_from_pyelftools(elf)
        if sizes is not None:
            return sizes, "pyelftools"
    sizes = sizes_from_footprint_json(build_dir)
    if sizes is not None:
        return sizes, "rom/ram.json"
    return None, None


# ---------------------------------------------------------------------
# SoM memory-budget resolution (SoC JSON, via the alp_project helpers)
# ---------------------------------------------------------------------


def _core_token(core_id: str) -> str:
    """``m55_hp`` -> ``M55_HP`` -- the token embedded in `sram_banks_kb`
    keys like ``SRAM3_M55_HP_DTCM``."""
    return core_id.upper()


def resolve_budget(
    sku: Optional[str],
    core_id: str,
    sdk_root: Path,
) -> tuple[Optional[int], Optional[int], Optional[str]]:
    """Resolve a slice's ``(flash_total, ram_total)`` budget in bytes.

    FLASH total is the SoM's on-die program flash (SoC variant `mram_mb`,
    else `soc_flash_mb`).  RAM total is the per-core data RAM: the
    matching ``*_DTCM`` bank from the variant's `sram_banks_kb`, else the
    core's `tcm_kb`.  Any field that can't be resolved comes back None
    (the caller renders ``unknown`` -- never a guessed number).

    The third element is a human note when a coarser fallback was used.
    """
    if not sku:
        return None, None, "no SKU in manifest"

    metadata_root = Path(sdk_root) / "metadata"
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if yaml is None or not preset_path.is_file():
        return None, None, f"no SoM preset for {sku}"
    try:
        preset = yaml.safe_load(preset_path.read_text(encoding="utf-8")) or {}
    except (OSError, ValueError):
        return None, None, f"unreadable SoM preset for {sku}"
    if "sku" not in preset:
        preset["sku"] = sku

    # Lazy import: alp_project pulls in jsonschema and wants scripts/ on
    # sys.path; keep it lazy so this module imports in lean contexts.
    try:
        from alp_project import _resolve_silicon_variant
    except ImportError:  # pragma: no cover - alp_project always present in-repo
        _resolve_silicon_variant = None  # type: ignore[assignment]

    variant: dict[str, Any] = {}
    if _resolve_silicon_variant is not None:
        variant = _resolve_silicon_variant(preset, metadata_root) or {}

    soc_spec: dict[str, Any] = {}
    silicon = preset.get("silicon") or ""
    parts = silicon.split(":")
    if len(parts) == 3:
        soc_path = metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"
        if soc_path.is_file():
            try:
                soc_spec = json.loads(soc_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                soc_spec = {}

    notes: list[str] = []

    # ---- FLASH: on-die program flash -------------------------------
    flash_total: Optional[int] = None
    mram_mb = variant.get("mram_mb")
    soc_flash_mb = soc_spec.get("soc_flash_mb")
    if isinstance(mram_mb, (int, float)):
        flash_total = int(mram_mb * 1024 * 1024)
    elif isinstance(soc_flash_mb, (int, float)):
        flash_total = int(soc_flash_mb * 1024 * 1024)
        notes.append("flash=soc_flash_mb")

    # ---- RAM: per-core data RAM ------------------------------------
    ram_total: Optional[int] = None
    token = _core_token(core_id)
    banks = variant.get("sram_banks_kb") or {}
    if isinstance(banks, dict):
        for name, kib in banks.items():
            if token in name and "DTCM" in name and isinstance(kib, (int, float)):
                ram_total = int(kib * 1024)
                break
    if ram_total is None:
        for core in soc_spec.get("cores") or []:
            if isinstance(core, dict) and core.get("id") == core_id:
                tcm_kb = core.get("tcm_kb")
                if isinstance(tcm_kb, (int, float)):
                    ram_total = int(tcm_kb * 1024)
                    notes.append("ram=core tcm_kb (ITCM+DTCM)")
                break

    note = "; ".join(notes) if notes else None
    return flash_total, ram_total, note


# ---------------------------------------------------------------------
# Per-slice report model
# ---------------------------------------------------------------------


@dataclass
class SliceSize:
    """One slice's measured footprint vs its resolved budget."""

    core_id: str
    os: str
    status: str                            # ok | over | warn | not-built | n/a | no-budget
    flash_used: Optional[int] = None
    flash_total: Optional[int] = None
    ram_used: Optional[int] = None
    ram_total: Optional[int] = None
    source: Optional[str] = None
    note: Optional[str] = None
    notes: list[str] = field(default_factory=list)

    @property
    def budget_known(self) -> bool:
        return self.flash_total is not None or self.ram_total is not None

    @property
    def over_budget(self) -> bool:
        return self.status == "over"

    def to_json_entry(self) -> dict[str, Any]:
        def region(used: Optional[int], total: Optional[int]) -> dict[str, Any]:
            pct = (round(used / total * 100, 1)
                   if used is not None and total else None)
            return {"used": used, "total": total, "pct": pct}

        entry: dict[str, Any] = {
            "core_id": self.core_id,
            "os": self.os,
            "status": self.status,
            "flash": region(self.flash_used, self.flash_total),
            "ram": region(self.ram_used, self.ram_total),
            "source": self.source,
        }
        if self.note:
            entry["budget_note"] = self.note
        if self.notes:
            entry["notes"] = list(self.notes)
        return entry


def _classify(flash_used, flash_total, ram_used, ram_total) -> str:
    """ok / warn / over from the worst of the two regions."""
    worst = "ok"
    for used, total in ((flash_used, flash_total), (ram_used, ram_total)):
        if used is None or not total:
            continue
        frac = used / total
        if frac > 1.0:
            return "over"
        if frac >= _WARN_FRACTION:
            worst = "warn"
    return worst


def build_slice_size(
    slice_: dict,
    build_root: Path,
    sku: Optional[str],
    sdk_root: Path,
    size_bin: Optional[str],
    run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> SliceSize:
    """Measure + budget one manifest slice into a SliceSize row."""
    core_id = str(slice_.get("core_id", "?"))
    os_ = str(slice_.get("os", "?"))

    if os_ != "zephyr":
        return SliceSize(core_id, os_, "n/a",
                         note="no Zephyr image (Yocto/baremetal)")

    elf = slice_elf_path(slice_, build_root)
    build_dir = slice_build_dir(slice_, build_root)
    sizes, source = extract_sizes(elf, build_dir, size_bin, run=run)

    flash_total, ram_total, budget_note = resolve_budget(sku, core_id, sdk_root)

    if sizes is None:
        row = SliceSize(core_id, os_, "not-built",
                        flash_total=flash_total, ram_total=ram_total,
                        note=budget_note)
        row.notes.append(f"no footprint source at {elf}")
        return row

    flash_used, ram_used = sizes
    if flash_total is None and ram_total is None:
        status = "no-budget"
    else:
        status = _classify(flash_used, flash_total, ram_used, ram_total)
    return SliceSize(
        core_id, os_, status,
        flash_used=flash_used, flash_total=flash_total,
        ram_used=ram_used, ram_total=ram_total,
        source=source, note=budget_note,
    )


def build_report(
    manifest: dict,
    build_root: Path,
    sdk_root: Path,
    sku: Optional[str] = None,
    size_bin: Optional[str] = None,
    run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
) -> list[SliceSize]:
    """Walk every manifest slice into a list of SliceSize rows."""
    sku = sku or (manifest.get("hw_info") or {}).get("sku")
    rows: list[SliceSize] = []
    for slice_ in manifest.get("slices") or []:
        if not isinstance(slice_, dict):
            continue
        rows.append(build_slice_size(
            slice_, build_root, sku, sdk_root, size_bin, run=run))
    return rows


# ---------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------


def _human(n: Optional[int]) -> str:
    """Bytes -> a compact KiB/MiB string, or ``?`` for unknown."""
    if n is None:
        return "?"
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f}M"
    if n >= 1024:
        return f"{n / 1024:.1f}K"
    return f"{n}B"


def _region_cell(used: Optional[int], total: Optional[int]) -> str:
    pct = (f"{used / total * 100:5.1f}%"
           if used is not None and total else "   -  ")
    return f"{_human(used):>8}/{_human(total):<8} {pct}"


_STATUS_HUE = {
    "ok": (lambda: Fore.GREEN, "OK"),
    "warn": (lambda: Fore.YELLOW, "WARN"),
    "over": (lambda: Fore.RED, "OVER"),
    "not-built": (lambda: Fore.YELLOW, "not built"),
    "n/a": (lambda: Fore.CYAN, "n/a"),
    "no-budget": (lambda: Fore.CYAN, "no budget"),
}


def _use_color(color: Optional[bool]) -> bool:
    if color is False:
        return False
    if color is True:
        return True
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


def render_table(rows: list[SliceSize], color: Optional[bool] = None) -> str:
    """Render the per-slice table as a multi-line string."""
    use_color = _use_color(color)

    def paint(s: str, hue: str) -> str:
        return f"{hue}{s}{Style.RESET_ALL}" if use_color else s

    head = (f"{'CORE':<14} {'OS':<10} "
            f"{'FLASH used/total':<24} {'RAM used/total':<24} STATUS")
    lines = [head, "-" * len(head)]
    for r in rows:
        hue_fn, label = _STATUS_HUE.get(r.status, (lambda: "", r.status))
        status = paint(label, hue_fn()) if use_color else label
        lines.append(
            f"{r.core_id:<14} {r.os:<10} "
            f"{_region_cell(r.flash_used, r.flash_total):<24} "
            f"{_region_cell(r.ram_used, r.ram_total):<24} {status}")
        detail = r.note or (r.notes[0] if r.notes else None)
        if detail:
            lines.append(f"{'':<14} {'':<10} -> {detail}")
    return "\n".join(lines)


def render_json(rows: list[SliceSize]) -> str:
    """Machine-readable form for the VS Code extension (like
    `alp doctor --json`)."""
    payload = {
        "schema": "alp-size/1",
        "slices": [r.to_json_entry() for r in rows],
        "summary": {
            "over_budget": sorted(r.core_id for r in rows if r.over_budget),
            "unknown_budget": sorted(
                r.core_id for r in rows
                if r.os == "zephyr" and r.status != "not-built"
                and not r.budget_known),
        },
    }
    return json.dumps(payload, indent=2, sort_keys=False)


def over_budget_rows(rows: list[SliceSize]) -> list[SliceSize]:
    return [r for r in rows if r.over_budget]


def unknown_budget_rows(rows: list[SliceSize]) -> list[SliceSize]:
    """Zephyr slices that were measured but whose budget couldn't be
    resolved -- skipped by `--fail-over-budget`."""
    return [r for r in rows
            if r.os == "zephyr" and r.status == "no-budget"]


# ---------------------------------------------------------------------
# west command
# ---------------------------------------------------------------------


class AlpSize(WestCommand):

    def __init__(self) -> None:
        super().__init__(
            "alp-size",
            "Report per-slice flash/RAM footprint against the SoM budget",
            "\n".join(__doc__.splitlines()[2:]) if __doc__ else "",
        )

    def do_add_parser(self, parser_adder):    # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument(
            "app_path", nargs="?", default=".",
            help="Path to the application source directory.")
        parser.add_argument(
            "--build-root", default=None,
            help="Override the build root (default: <app_path>/build).")
        parser.add_argument(
            "--board", default=None,
            help="Override the SoM SKU used to resolve the memory budget "
                 "(default: hw_info.sku from the manifest).")
        parser.add_argument(
            "--json", action="store_true",
            help="Emit the machine-readable report (for the VS Code "
                 "extension), instead of the human table.")
        parser.add_argument(
            "--fail-over-budget", action="store_true",
            help="Exit non-zero if any slice exceeds its resolved budget "
                 "(slices with an unknown budget are skipped + reported).")
        return parser

    def do_run(self, args, _unknown):        # type: ignore[no-untyped-def]
        sdk_root = find_sdk_root()
        if sdk_root is None:
            log.die("Cannot locate alp-sdk root.")
            return 1

        app_path = Path(args.app_path).resolve()
        build_root = (Path(args.build_root).resolve()
                      if args.build_root
                      else app_path / "build")

        try:
            manifest = load_manifest(build_root)
        except AlpSizeError as e:
            log.die(str(e))
            return 1

        sku = args.board or (manifest.get("hw_info") or {}).get("sku")
        size_bin = find_size_tool()
        rows = build_report(manifest, build_root, sdk_root,
                            sku=sku, size_bin=size_bin)

        if args.json:
            print(render_json(rows))
        else:
            print(render_table(rows))
            if size_bin is None:
                log.inf("alp-size: no size tool on PATH "
                        "(arm-zephyr-eabi-size / llvm-size / size); "
                        "fell back to pyelftools / rom.json+ram.json.")

        if args.fail_over_budget:
            over = over_budget_rows(rows)
            unknown = unknown_budget_rows(rows)
            if unknown and not args.json:
                names = ", ".join(r.core_id for r in unknown)
                log.inf(f"alp-size: budget unknown for [{names}] -- "
                        f"skipped by --fail-over-budget (no guess).")
            if over:
                names = ", ".join(r.core_id for r in over)
                log.die(f"alp-size: over budget: [{names}].")
                return 1
        return 0
