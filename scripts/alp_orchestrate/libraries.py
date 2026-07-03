# SPDX-License-Identifier: Apache-2.0
"""Curated third-party library layer (ADR 0018).

A project selects libraries with one top-level key::

    libraries: [lvgl, cmsis-dsp, nanopb]

Each name resolves to a manifest at ``metadata/libraries/<name>.yaml`` (the
single source of truth, validated by ``scripts/validate_metadata.py``).  This
module is the emit-side half: it loads the manifests, validates each selection
against the resolved SoM/core capabilities, and yields the per-OS wiring the
slice emitters (``kconfig.py``) splice into their fragments -- Zephyr Kconfig,
Yocto ``IMAGE_INSTALL``, baremetal cmake args -- through the ADR 0014 --emit
contract.  No forking, no fetching: west/OE own the actual pin (ADR 0017).

Design invariants:

* **Zero-diff when unused.**  A project that declares no ``libraries:`` gets
  byte-identical emit output -- every helper returns ``[]`` for an empty
  selection and the emitters guard on that.
* **Fail early, name the constraint.**  An incompatible selection raises
  ``OrchestratorError`` with the failing ``requires:`` constraint named, the
  same clear-error contract as schema validation, so ``alp doctor`` /
  alp-studio (which read the same manifests) can never disagree with emit.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

import yaml

from alp_project import resolve_capabilities

from .models import BoardProject, OrchestratorError, Slice
from .paths import METADATA_ROOT


def _libraries_dir(metadata_root: Path) -> Path:
    return metadata_root / "libraries"


def available_libraries(metadata_root: Path = METADATA_ROOT) -> list[str]:
    """Sorted list of every curated library token (manifest filename stem)."""
    d = _libraries_dir(metadata_root)
    if not d.is_dir():
        return []
    return sorted(p.stem for p in d.glob("*.yaml"))


def load_manifest(name: str, metadata_root: Path = METADATA_ROOT) -> dict[str, Any]:
    """Load one library manifest, or raise OrchestratorError naming the options.

    The error lists every available library so a typo in ``libraries: [...]``
    is self-correcting.
    """
    path = _libraries_dir(metadata_root) / f"{name}.yaml"
    if not path.is_file():
        options = ", ".join(available_libraries(metadata_root)) or "<none>"
        raise OrchestratorError(
            f"unknown library `{name}` in `libraries:` -- no manifest at "
            f"metadata/libraries/{name}.yaml.  Available: {options}")
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict):
        raise OrchestratorError(
            f"library manifest metadata/libraries/{name}.yaml is not a mapping")
    return doc


# ---------------------------------------------------------------------------
# Target-capability resolution (the `requires:` right-hand side)
# ---------------------------------------------------------------------------

def _core_classes(project: BoardProject) -> set[str]:
    """The set of application-core classes the SoC provides: {"m"}, {"a"}, ...

    Derived from the SoC spec's ``cores[].type`` (``cortex-m*`` -> m,
    ``cortex-a*`` -> a), matching how the loader picks a default OS per core.
    """
    classes: set[str] = set()
    for core in project.soc_spec.get("cores", []) or []:
        ctype = str(core.get("type", "")).lower()
        if ctype.startswith("cortex-m"):
            classes.add("m")
        elif ctype.startswith("cortex-a"):
            classes.add("a")
    return classes


def _core_class_of(project: BoardProject, slice_: Slice) -> Optional[str]:
    """The class (m|a) of a specific slice's core, from the SoC spec."""
    for core in project.soc_spec.get("cores", []) or []:
        if core.get("id") == slice_.core_id:
            ctype = str(core.get("type", "")).lower()
            if ctype.startswith("cortex-m"):
                return "m"
            if ctype.startswith("cortex-a"):
                return "a"
    return None


def _project_oses(project: BoardProject) -> set[str]:
    """OSes actually running on some core (excludes `off`)."""
    return {s.os for s in project.cores.values() if s.os and s.os != "off"}


def _check_requires(
    name: str,
    manifest: dict[str, Any],
    project: BoardProject,
    metadata_root: Path,
) -> None:
    """Raise OrchestratorError if the project fails the manifest's `requires:`.

    Every failure names the exact constraint and the observed target value so
    the message is actionable ("... needs core_class m but SoM E1M-... has {a}").
    """
    requires = manifest.get("requires") or {}
    if not isinstance(requires, dict):
        return

    caps = resolve_capabilities(project.som_preset, metadata_root)

    for cap in requires.get("capabilities") or []:
        if not caps.get(cap):
            raise OrchestratorError(
                f"library `{name}` requires capability `{cap}`, which SoM "
                f"`{project.sku}` does not provide")

    min_ram = requires.get("min_ram_kib")
    if min_ram is not None:
        soc_ram = project.soc_spec.get("soc_ram_kb")
        if soc_ram is not None and float(soc_ram) < float(min_ram):
            raise OrchestratorError(
                f"library `{name}` requires min_ram_kib {min_ram}, but SoC for "
                f"SoM `{project.sku}` has {soc_ram} KB RAM")

    min_flash = requires.get("min_flash_kib")
    if min_flash is not None:
        soc_flash_mb = project.soc_spec.get("soc_flash_mb")
        if soc_flash_mb is not None and float(soc_flash_mb) * 1024 < float(min_flash):
            raise OrchestratorError(
                f"library `{name}` requires min_flash_kib {min_flash}, but SoC "
                f"for SoM `{project.sku}` has {soc_flash_mb} MB flash")

    core_class = requires.get("core_class")
    if core_class is not None:
        have = _core_classes(project)
        if core_class not in have:
            raise OrchestratorError(
                f"library `{name}` requires core_class `{core_class}`, but SoM "
                f"`{project.sku}` has core class(es) {sorted(have) or '<none>'}")

    req_os = requires.get("os")
    if req_os:
        have_os = _project_oses(project)
        if not (set(req_os) & have_os):
            raise OrchestratorError(
                f"library `{name}` requires os {sorted(req_os)}, but this "
                f"project's cores run {sorted(have_os) or '<none>'}")


def resolve_selection(
    project: BoardProject,
    metadata_root: Path = METADATA_ROOT,
) -> list[tuple[str, dict[str, Any]]]:
    """Validate every selected library and return [(name, manifest), ...].

    Raises OrchestratorError on an unknown name, a failed `requires:`
    constraint, or a library whose manifest has no `integration:` section for
    ANY OS this project's cores run (you asked for a library that cannot be
    wired anywhere on this target).  Returns [] for an empty selection.
    """
    selected = list(project.libraries or [])
    resolved: list[tuple[str, dict[str, Any]]] = []
    project_oses = _project_oses(project)
    for name in selected:
        manifest = load_manifest(name, metadata_root)
        _check_requires(name, manifest, project, metadata_root)
        integration = manifest.get("integration") or {}
        wireable = set(integration.keys()) & project_oses
        if project_oses and not wireable:
            have_sections = ", ".join(sorted(integration.keys())) or "<none>"
            raise OrchestratorError(
                f"library `{name}` cannot be wired on this project: its manifest "
                f"has integration section(s) [{have_sections}] but this project's "
                f"cores run {sorted(project_oses)}")
        resolved.append((name, manifest))
    return resolved


# ---------------------------------------------------------------------------
# Per-slice emit helpers (called from kconfig.py)
# ---------------------------------------------------------------------------

def zephyr_kconfig_lines(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path = METADATA_ROOT,
) -> list[str]:
    """Kconfig lines for every top-level library with a Zephyr integration.

    Emitted into a Zephyr slice's alp.conf.  Returns [] when the project
    selects no libraries (keeps existing emit byte-identical).
    """
    if not project.libraries or slice_.os != "zephyr":
        return []
    lines: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root):
        zephyr = (manifest.get("integration") or {}).get("zephyr")
        if not zephyr:
            continue
        _check_core_class(name, manifest, project, slice_)
        module = zephyr.get("module")
        tag = f"{name} v{manifest.get('version', '?')}"
        if module:
            tag += f" (west module `{module}`)"
        lines.append(f"# library: {tag}")
        for kc in zephyr.get("kconfig") or []:
            lines.append(kc)
    return lines


def yocto_image_install(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path = METADATA_ROOT,
) -> list[str]:
    """IMAGE_INSTALL recipe names for libraries with a Yocto integration."""
    if not project.libraries or slice_.os != "yocto":
        return []
    packages: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root):
        yocto = (manifest.get("integration") or {}).get("yocto")
        if not yocto:
            continue
        packages.extend(yocto.get("image_install") or [])
    return packages


def baremetal_cmake_args(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path = METADATA_ROOT,
) -> list[str]:
    """cmake hint lines for libraries with a baremetal integration."""
    if not project.libraries or slice_.os != "baremetal":
        return []
    args: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root):
        baremetal = (manifest.get("integration") or {}).get("baremetal")
        if not baremetal:
            continue
        cmake = baremetal.get("cmake")
        if cmake:
            args.append(f"# library {name}: {cmake}")
    return args


def _check_core_class(
    name: str,
    manifest: dict[str, Any],
    project: BoardProject,
    slice_: Slice,
) -> None:
    """A per-slice guard: a core_class-constrained library must not be wired
    onto a core of the wrong class, even if some OTHER core satisfies the
    project-wide `requires:` check.
    """
    core_class = (manifest.get("requires") or {}).get("core_class")
    if core_class is None:
        return
    have = _core_class_of(project, slice_)
    if have is not None and have != core_class:
        raise OrchestratorError(
            f"library `{name}` requires core_class `{core_class}` but is "
            f"selected on core `{slice_.core_id}` (class {have})")
