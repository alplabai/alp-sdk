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
  same clear-error contract as schema validation, so ``tan doctor`` /
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


def available_libraries(metadata_root: Path) -> list[str]:
    """Sorted list of every curated library token (manifest filename stem)."""
    d = _libraries_dir(metadata_root)
    if not d.is_dir():
        return []
    return sorted(p.stem for p in d.glob("*.yaml"))


def load_manifest(name: str, metadata_root: Path) -> dict[str, Any]:
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


def scoped_names(
    project: BoardProject,
    slice_: Optional[Slice] = None,
) -> list[str]:
    """Every library name in scope, in declaration order, deduped.

    board.yaml declares each curated library ONCE, in the top-level
    `libraries:` list.  `loader._normalize_libraries` then folds each entry
    into one of two internal channels: an entry with no `cores:` lands in
    `project.libraries`, an entry with `cores:` lands in each named
    `Slice.libraries`.  Both channels are the SAME ADR-0018 selection and
    must pass through the SAME checks -- reading only `project.libraries`
    here is what let a core-scoped entry skip `load_manifest`'s unknown-name
    refusal, its `requires:` constraints and its `integration:` sections
    entirely (alplabai/tan-cli#555).

    ``slice_`` None  -> project-wide + every core-scoped name declared on any
                        core: the whole-project view `alp_project.py`'s
                        up-front `resolve_selection` check (before any
                        `--emit`) validates.
    ``slice_`` given -> project-wide + that one slice's core-scoped names:
                        what the per-slice emitters wire.
    """
    names: list[str] = list(project.libraries or [])
    slices = [slice_] if slice_ is not None else list(project.cores.values())
    for s in slices:
        for name in (s.libraries or []):
            if name not in names:
                names.append(name)
    return names


def resolve_selection(
    project: BoardProject,
    metadata_root: Path,
    *,
    slice_: Optional[Slice] = None,
) -> list[tuple[str, dict[str, Any]]]:
    """Validate every selected library and return [(name, manifest), ...].

    Raises OrchestratorError on an unknown name, a failed `requires:`
    constraint, or a library whose manifest has no `integration:` section for
    ANY OS this project's cores run (you asked for a library that cannot be
    wired anywhere on this target).  Returns [] for an empty selection.

    The selection covers BOTH declaration channels -- see `scoped_names` for
    what ``slice_`` narrows it to.

    `requires:` is checked at TWO altitudes, because the two channels ask
    different questions.  `_check_requires` is project-wide: a project-wide
    `libraries: [ros2]` on a Yocto-A55 + Zephyr-M33 board is legitimate and
    simply wires on the A55 only.  A CORE-SCOPED entry (`{name: ros2, cores:
    [m33_sm]}`) is instead an explicit instruction to wire it THERE, so it
    additionally goes through `_check_slice_requires` against that one slice
    (alplabai/tan-cli#555: "ros2's `requires: {os: [yocto], core_class: a}`
    is never checked, so ros2 scoped to a Cortex-M core is silently
    no-op'd").  The project-wide check cannot catch it -- `_project_oses`
    contains `yocto` from the A55, so it passes.
    """
    selected = scoped_names(project, slice_)
    core_scoped = set(slice_.libraries or ()) if slice_ is not None else set()
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
        if name in core_scoped:
            assert slice_ is not None  # core_scoped is empty when slice_ is None
            _check_slice_requires(name, manifest, project, slice_)
        resolved.append((name, manifest))
    return resolved


def _check_slice_requires(
    name: str,
    manifest: dict[str, Any],
    project: BoardProject,
    slice_: Slice,
) -> None:
    """`requires:` for a library board.yaml explicitly scoped to THIS core.

    `cores: [<id>]` is a positive instruction, not a hint, so a `requires:`
    key that the named core violates has to fail early and name the
    constraint rather than resolve to nothing.  Checks the two keys that are
    per-core facts:

      * `os:`         -- against this slice's own OS, not the project-wide
                         union `_check_requires` uses.
      * `core_class:` -- delegated to `_check_core_class`, which used to be
                         reachable only from `zephyr_kconfig_lines` AFTER an
                         `if not zephyr: continue`, so a manifest with no
                         `integration.zephyr:` (ros2) never reached it.

    Deliberately does NOT refuse a manifest that merely has no
    `integration.<this os>:` section: mbedtls and nlohmann-json are scoped to
    the Yocto cores of four shipped examples and have no `integration.yocto:`,
    and the honest handling for those is the explanatory `yocto_unwireable`
    comment, not a hard error.
    """
    requires = manifest.get("requires") or {}
    req_os = requires.get("os")
    if req_os and slice_.os not in set(req_os):
        raise OrchestratorError(
            f"library `{name}` is scoped to core `{slice_.core_id}` "
            f"(os `{slice_.os}`) but requires os {sorted(req_os)}")
    _check_core_class(name, manifest, project, slice_)


# ---------------------------------------------------------------------------
# Per-slice emit helpers (called from kconfig.py)
# ---------------------------------------------------------------------------

def zephyr_library_kconfig(manifest: dict[str, Any]) -> list[str]:
    """Kconfig lines for ONE library manifest's Zephyr integration.

    The upstream module-enable line(s) (`integration.zephyr.kconfig`)
    followed by the SDK SW-fallback floor
    (`integration.zephyr.hw_backends.sw_fallback.kconfig`), module-enable
    first and the fallback marker last, deduped. Header-only libraries whose
    SW floor is a doc comment surface that comment only when there is no
    module-enable line to attach it to.

    The ONE derivation both declaration channels call for the base+fallback
    set -- `zephyr_kconfig_lines` (project-wide) and
    `kconfig.py::_per_core_library_kconfig` (core-scoped) -- so the same
    library manifest can never emit a different symbol set depending on
    which `libraries:` channel declared it (#1359: the project-wide channel
    used to skip the SW-fallback line entirely, e.g. cmsis-dsp's project-wide
    form never emitted `CONFIG_ALP_CMSIS_DSP_SCALAR=y` while its core-scoped
    form did).
    """
    zephyr = (manifest.get("integration") or {}).get("zephyr") or {}
    out: list[str] = []
    for kc in zephyr.get("kconfig") or []:
        kc = str(kc)
        if kc not in out:
            out.append(kc)
    swf = ((zephyr.get("hw_backends") or {}).get("sw_fallback") or {}).get("kconfig")
    if swf:
        swf = str(swf)
        if swf.lstrip().startswith("#"):
            # Header-only library: the SW floor is a documentation comment,
            # not a real knob.  Surface it only when there is no module enable.
            if not out:
                out.append(swf)
        elif swf not in out:
            out.append(swf)
    return out


def zephyr_kconfig_lines(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path,
) -> list[str]:
    """Kconfig lines for every project-wide library with a Zephyr integration.

    Emitted into a Zephyr slice's alp.conf.  Returns [] when the project
    selects no libraries (keeps existing emit byte-identical).

    Both channels are RESOLVED here (so a core-scoped entry gets the same
    unknown-name / `requires:` / wireability / core-class refusals as a
    project-wide one), but only the project-wide half is EMITTED: the Zephyr
    Kconfig for a core-scoped entry rides `kconfig.py::_emit_libraries`'
    per-core channel, which reads the very same manifest.  Emitting it twice
    would duplicate every CONFIG_ line.  Both halves derive their per-library
    CONFIG_ set from the same `zephyr_library_kconfig` helper (#1359).
    """
    if slice_.os != "zephyr":
        return []
    project_wide = set(project.libraries or ())
    lines: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root, slice_=slice_):
        zephyr = (manifest.get("integration") or {}).get("zephyr")
        if not zephyr:
            continue
        _check_core_class(name, manifest, project, slice_)
        if name not in project_wide:
            continue
        module = zephyr.get("module")
        version = str(manifest.get("version", "?"))
        display_version = version if version.startswith("v") else f"v{version}"
        tag = f"{name} {display_version}"
        if module:
            tag += f" (west module `{module}`)"
        lines.append(f"# library: {tag}")
        for kc in zephyr_library_kconfig(manifest):
            lines.append(kc)
    return lines


def yocto_image_install(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path,
) -> list[str]:
    """IMAGE_INSTALL recipe names for libraries with a Yocto integration.

    Covers BOTH declaration channels: a `libraries:` entry scoped to this
    Yocto core contributes its manifest's `integration.yocto.image_install`
    recipes, exactly like a project-wide one.  The recipe names come from the
    manifest and nowhere else -- they are never derived from the library name
    (alplabai/tan-cli#555: the old per-core path invented `lib-<name>`, which
    RPROVIDES nothing in meta-alp-sdk or OE).
    """
    if slice_.os != "yocto":
        return []
    packages: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root, slice_=slice_):
        yocto = (manifest.get("integration") or {}).get("yocto")
        if not yocto:
            continue
        for pkg in yocto.get("image_install") or []:
            if pkg not in packages:
                packages.append(pkg)
    return packages


def yocto_unwireable(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path,
) -> list[str]:
    """In-scope library names whose manifest has NO `integration.yocto:`.

    Such a library contributes nothing to this slice's `IMAGE_INSTALL` -- the
    honest emit is nothing.  The caller surfaces the names as a comment so the
    no-op is visible in the generated local.conf instead of silent.
    """
    if slice_.os != "yocto":
        return []
    return [
        name
        for name, manifest in resolve_selection(project, metadata_root, slice_=slice_)
        if not (manifest.get("integration") or {}).get("yocto")
    ]


def baremetal_cmake_args(
    project: BoardProject,
    slice_: Slice,
    metadata_root: Path,
) -> list[str]:
    """cmake hint lines for libraries with a baremetal integration.

    Covers both declaration channels (see `scoped_names`)."""
    if slice_.os != "baremetal":
        return []
    args: list[str] = []
    for name, manifest in resolve_selection(project, metadata_root, slice_=slice_):
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
