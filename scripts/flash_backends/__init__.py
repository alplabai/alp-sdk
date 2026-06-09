# SPDX-License-Identifier: Apache-2.0
"""
Flash-backend registry for `west alp-flash` (Wave 5B of the 2026-05-15
heterogeneous-OS orchestration design).

Phase 2 wrote `flash_method:` + `flash_args:` into every slice + helper
MCU entry of build/system-manifest.yaml; this package turns those
strings into real subprocess invocations.

Public API:

    FlashContext, FlashResult, FlashBackend, REGISTRY,
    register(backend), lookup(method)

Each backend module under this package self-registers on import via
``register(<ThisBackend>())``; importing the package therefore
populates the registry.

The orchestrator's ``_slice_flash_recipe`` emits these method names:

    yocto:     "yocto_wic_to_sd_or_emmc"
    zephyr:    "zephyr_west_flash"
    baremetal: "baremetal_cmake_flash"

and helper-MCU entries from SoM presets emit (e.g.):

    GD32 supervisor: "swd_probe"
    CC3501E coproc:  "cc3501e_usb_bootloader"

The yocto backend also registers under the short alias ``yocto_wic``
so direct test invocations and manifest hand-edits stay terse.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Protocol, runtime_checkable


@dataclass
class FlashContext:
    """Inputs every backend receives.

    Attributes:
      artefact_path  Absolute path to the slice's output artefact
                     (`.wic` / `.elf` / `.bin` / `.hex` / `.uf2`).
      flash_args     The ``flash_args`` dict copied verbatim from
                     the manifest slice (or helper_mcus entry).
      core_id        Identifier of the slice this call programs;
                     used only for human-readable log lines.
      sku            SoM SKU (e.g. ``E1M-V2N101``).  Some backends
                     pick interface configs off the SKU.
      sdk_root       Optional alp-sdk root path -- backends use it
                     to resolve relative scripts (e.g. an openocd
                     interface file shipped under scripts/).
      dry_run        When True, the backend prints what it WOULD
                     run and returns ``ok=True`` without invoking
                     subprocess.  Used by ``--dry-run``.
    """

    artefact_path: Path
    flash_args: dict
    core_id: str
    sku: str
    sdk_root: Optional[Path] = None
    dry_run: bool = False


@dataclass
class FlashResult:
    """Per-slice outcome returned by every backend.

    Attributes:
      ok        True if the backend succeeded (or dry-ran cleanly).
      elapsed_s Wall-clock seconds the backend spent.
      message   Human-readable summary -- on failure this carries
                the reason (missing tool, non-zero rc, stderr tail).
      command   The argv list that was (or would have been) invoked.
                Helps the caller print a "would run X" line.
    """

    ok: bool
    elapsed_s: float
    message: str
    command: list[str] = field(default_factory=list)


@runtime_checkable
class FlashBackend(Protocol):
    """Interface every backend module implements.

    Implementations live in sibling modules (one backend per file)
    and end with ``register(<ThisBackend>())``.
    """

    name: str
    """Registry key.  Matches the ``flash_method`` string the
    orchestrator writes into build/system-manifest.yaml."""

    requires: list[str]
    """External executables this backend needs on $PATH.  The
    dispatch layer checks each via ``shutil.which`` before
    invoking the backend; a single missing tool surfaces as a
    skipped slice (under ``--skip-missing-tools``) or a hard fail."""

    def flash(self, ctx: FlashContext) -> FlashResult:
        """Program ``ctx.artefact_path`` according to ``ctx.flash_args``."""
        ...


REGISTRY: dict[str, "FlashBackend"] = {}


def register(backend: "FlashBackend") -> None:
    """Add a backend to the global registry under ``backend.name``."""
    REGISTRY[backend.name] = backend


def lookup(method: str) -> Optional["FlashBackend"]:
    """Return the registered backend for ``method``, or ``None``."""
    return REGISTRY.get(method)


# Eagerly import every backend module so the registry is populated
# the moment a caller does `import scripts.flash_backends`.  Each
# module ends with `register(<ThisBackend>())`.
#
# Listed here (rather than discovered via pkgutil.iter_modules) so the
# load order is deterministic + the registry is fully populated even
# under zip-imports.
from . import yocto_wic                  # noqa: E402,F401
from . import zephyr_west_flash          # noqa: E402,F401
from . import baremetal_cmake_flash      # noqa: E402,F401
from . import swd_probe                  # noqa: E402,F401
from . import cc3501e_usb_bootloader     # noqa: E402,F401
from . import xspi_flashwriter            # noqa: E402,F401


__all__ = [
    "FlashContext",
    "FlashResult",
    "FlashBackend",
    "REGISTRY",
    "register",
    "lookup",
]
