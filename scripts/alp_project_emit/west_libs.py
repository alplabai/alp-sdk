#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Library HW-backend binding + west.yml fragment emission
(`--emit west-libraries` and the `libraries:` HW-accelerator loader hook).

Split out of the former flat `alp_project_emit.py` module (issue #673
Phase 1) -- see `scripts/alp_project_emit/__init__.py` for the package-
level contract.  Structural split only, no behaviour change;
`check_emit_snapshots.py` pins the byte-identical output.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: PyYAML is required.  Install via `pip install pyyaml`.")

from alp_project_loader import (
    METADATA_ROOT,
    _load_yaml,
    _sku_family,
    resolve_capabilities,
)


# §D.lib.loader: map _sku_family() return values to the soc_family
# tokens used in metadata/library-profiles/<name>/hw-backends.yaml.
_SOC_FAMILY_TOKEN: dict[str, str] = {
    "aen":    "alif_ensemble",
    "v2n":    "renesas_rzv2n",
    "v2n-m1": "renesas_rzv2n",     # DEEPX add-on; HW-acc tokens still resolve via host family.
    "imx93":  "nxp_imx9",
}


def _emit_library_hw_backends(libs: list[str], sku: str) -> list[str]:
    """Per-library HW-accelerator binding loader.

    For each enabled library that ships a
    `metadata/library-profiles/<name>/hw-backends.yaml`, pick the
    highest-priority implemented matching backend per accelerator class
    given the active SoM SKU and emit the matching `CONFIG_*=y` line.

    Match rules (per priority entry; checked in order, all specified
    keys must match):
      - `silicon:` key, if present, must equal the SKU's `silicon:`
        ref exactly (e.g. `alif:ensemble:e4`).  Lets us pin a backend
        to specific SoCs in a family -- Ethos-U85 on E4/E6/E8, but
        not on E3/E5/E7 which carry only U55.
      - `soc_family:` key, if present, must equal the SKU family
        token (`alif_ensemble`, `renesas_rzv2n`, ...).  Selects every
        SKU in that family.
      - `requires_cap:` key, if present, must name a capability flag
        in the SKU's `metadata/e1m_modules/<sku>.yaml` `capabilities:`
        block that resolves to a truthy value (`true` or a non-zero
        count).  Cleanest matcher when an accelerator is shared
        across families (e.g. `optiga_trust_m` is populated on AEN
        + V2N).
      - All three omitted = universal entry (e.g. plain FPU, generic
        DMA), matches any SKU.
      - `status: planned` / `status: stub` entries are retained as
        metadata for the roadmap but are not emitted as active build
        claims.
    """
    import re
    from pathlib import Path

    family       = _sku_family(sku)
    soc_token    = _SOC_FAMILY_TOKEN.get(family)
    if soc_token is None:
        return []

    out: list[str] = []
    repo_root = Path(__file__).resolve().parent.parent.parent

    # Resolve the SKU's `silicon:` ref and merged capabilities (SoC JSON
    # defaults + SoM-level overrides) via resolve_capabilities().  This
    # replaces the former inline YAML text-parser so that silicon-determined
    # capabilities removed from SoM YAMLs (Task 3, slice 3b) continue to
    # resolve from the SoC JSON that sibling Agent D populated.
    silicon_ref: str | None = None
    sku_path = repo_root / "metadata" / "e1m_modules" / f"{sku}.yaml"
    sku_preset: dict[str, Any] = {}
    if sku_path.exists():
        sku_preset = _load_yaml(sku_path) or {}
        silicon_ref = sku_preset.get("silicon")

    merged_caps: dict[str, Any] = resolve_capabilities(sku_preset, repo_root / "metadata")

    def _cap_truthy(name: str) -> bool:
        v = merged_caps.get(name)
        if v is None:
            return False
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return v > 0
        # String value from a YAML-loaded dict (should not occur after
        # resolve_capabilities, but guard for safety).
        sv = str(v).lower()
        if sv in ("true", "yes"):
            return True
        if sv in ("false", "no", "null", "none", "0"):
            return False
        try:
            return int(sv) > 0
        except ValueError:
            return False

    for lib in libs:
        prof = repo_root / "metadata" / "library-profiles" / lib / "hw-backends.yaml"
        if not prof.exists():
            continue
        try:
            text = prof.read_text(encoding="utf-8")
        except OSError:
            continue

        # Cheap line-driven parse: every `      - { ... kconfig: CONFIG_X=y }`
        # entry sits inside a `priority:` block; we walk top-down to keep
        # the per-class first-match.  No yaml dependency on the loader.
        per_class_emitted: set[str] = set()
        current_class: str | None   = None
        for raw in text.splitlines():
            cls_match = re.match(r"^\s*-\s*class:\s*(\S+)", raw)
            if cls_match:
                current_class = cls_match.group(1)
                continue
            if current_class is None or current_class in per_class_emitted:
                continue
            entry = re.match(r"^\s*-\s*\{\s*(.+)\s*\}\s*$", raw)
            if not entry:
                continue
            kv: dict[str, str] = {}
            for tok in entry.group(1).split(","):
                tok = tok.strip()
                if not tok or ":" not in tok:
                    continue
                k, v = tok.split(":", 1)
                kv[k.strip()] = v.strip()
            sili = kv.get("silicon")
            sf   = kv.get("soc_family")
            cap  = kv.get("requires_cap")
            kcv  = kv.get("kconfig")
            status = kv.get("status", "implemented").strip().lower()
            if not kcv:
                continue
            if status in {"planned", "stub"}:
                continue
            # All specified matchers must succeed.
            if sili is not None and sili != silicon_ref:
                continue
            if sf is not None and sf != soc_token:
                continue
            if cap is not None and not _cap_truthy(cap):
                continue
            out.append(f"{kcv}  # {lib} / {current_class}")
            per_class_emitted.add(current_class)

    return out


# ---------------------------------------------------------------------
# west.yml fragment emission (libraries -> Zephyr-module name-allowlist)
# ---------------------------------------------------------------------
#
# Closes the second v0.4 gap docs/board-config.md flagged: customers
# whose board.yaml declares `libraries: [lvgl, mbedtls]` should not
# also have to hand-add those modules to their app's west.yml
# `name-allowlist:`.  The emitter produces a paste-ready fragment
# they import via a self-referencing `import:` block.


# Library name -> Zephyr module name the workspace's west.yml must
# import.  Mirrors zephyr/modules.git's published modules; LittleFS
# ships as `fs/littlefs` while the rest match their library names 1:1.
_LIBRARY_WEST_MODULES: dict[str, str] = {
    "lvgl":          "lvgl",
    "mbedtls":       "mbedtls",
    "cmsis_dsp":     "cmsis-dsp",
    "littlefs":      "fs/littlefs",
    # The four header-only C++ libraries (etl / fmt / nlohmann_json /
    # doctest) are not Zephyr modules today -- they land in v0.4 via
    # the per-library profile + include-path hook in the loader, not
    # via west.yml.  Listing them here would emit an entry that
    # `west update` rejects.
}


# OTA provider -> Zephyr module name the workspace's west.yml must
# import.  Hawkbit and MCUmgr ship in Zephyr upstream so no entry --
# only out-of-tree clients need a west.yml line.  See ADR 0009.
_OTA_PROVIDER_WEST_MODULES: dict[str, str] = {
    "mender":  "mender-mcu-client",
    # hawkbit -- in Zephyr upstream
    # mcumgr  -- in Zephyr upstream
}


def _load_curated_library_manifest(lib: str) -> dict[str, Any] | None:
    """Load a top-level ADR 0018 library manifest if one exists."""
    path = METADATA_ROOT / "libraries" / f"{lib}.yaml"
    if not path.is_file():
        return None
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    return doc if isinstance(doc, dict) else None


def _emit_west_libraries(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    *,
    v2_libraries: list[str] | None = None,
    v2_project_libraries: list[str] | None = None,
) -> str:
    """Emit a west.yml fragment that the customer's manifest can
    import to pin the Zephyr modules board.yaml's `libraries:` array
    requires.  Idempotent: emitting an empty `libraries:` array gives
    an empty (but well-formed) name-allowlist.

    v1 path (`v2_libraries is None`): reads project-level `libraries:`.
    v2 path: callers compute the union across the Zephyr-runtime cores
    (or pick one when `--core <id>` is supplied) and pass it in via
    `v2_libraries`.  `v2_project_libraries` carries the top-level ADR 0018
    curated library manifests; these may either import a Zephyr-owned module
    by name or emit a standalone west project pin from the manifest's
    `integration.zephyr.west` block.
    """
    del sku_preset, board_preset  # unused -- libraries are SoM-agnostic
    if v2_libraries is not None:
        libs = list(v2_libraries)
    else:
        libs = project.get("libraries") or []
    project_libs = list(v2_project_libraries
                        if v2_project_libraries is not None
                        else [])
    modules: list[tuple[str, str]] = []   # (library, Zephyr-owned west module)
    west_projects: list[tuple[str, dict[str, Any]]] = []
    unsupported: list[str] = []
    seen_modules: set[str] = set()
    seen_projects: set[str] = set()

    def add_module(lib: str, mod: str) -> None:
        if mod not in seen_modules:
            modules.append((lib, mod))
            seen_modules.add(mod)

    def add_west_project(lib: str, west: dict[str, Any]) -> None:
        name = str(west.get("name") or "")
        if name and name not in seen_projects:
            west_projects.append((lib, west))
            seen_projects.add(name)

    for lib in libs:
        mod = _LIBRARY_WEST_MODULES.get(lib)
        if mod is None:
            unsupported.append(lib)
        else:
            add_module(lib, mod)

    for lib in project_libs:
        manifest = _load_curated_library_manifest(lib)
        zephyr = ((manifest or {}).get("integration") or {}).get("zephyr") or {}
        if not zephyr:
            continue
        west = zephyr.get("west")
        if isinstance(west, dict):
            add_west_project(lib, west)
            continue
        mod = zephyr.get("module")
        if isinstance(mod, str) and mod:
            add_module(lib, mod)
        else:
            unsupported.append(lib)

    # OTA provider-driven dispatch (ADR 0009 follow-up): out-of-tree
    # Zephyr OTA clients need their own west.yml entry.  Mender-MCU-client
    # is the only one today; hawkbit and mcumgr ship in Zephyr upstream.
    ota = project.get("ota") or {}
    if isinstance(ota, dict):
        ota_provider = (ota.get("provider") or "").lower()
        ota_mod = _OTA_PROVIDER_WEST_MODULES.get(ota_provider)
        if ota_mod is not None:
            modules.append((f"ota:{ota_provider}", ota_mod))

    lines: list[str] = []
    lines.append("# SPDX-License-Identifier: Apache-2.0")
    lines.append("#")
    lines.append("# Auto-generated by scripts/alp_project.py -- "
                 "do not edit by hand.")
    lines.append("# Regenerate after changes to board.yaml's `libraries:` array.")
    lines.append("#")
    lines.append("# Import into your application's west.yml so `west update`")
    lines.append("# pulls only the Zephyr modules the libraries you enabled")
    lines.append("# actually need.  Drop alongside your west.yml and reference")
    lines.append("# from the `import:` field of the alp-sdk project entry.")
    lines.append("")
    lines.append("manifest:")
    lines.append("  projects:")
    lines.append("    - name: zephyr")
    lines.append("      import:")
    lines.append("        name-allowlist:")
    if modules:
        for lib, mod in modules:
            lines.append(f"          - {mod}        # board.yaml libraries: '{lib}'")
    else:
        lines.append("          # no selected Zephyr-owned modules -- nothing to allowlist.")
        lines.append("          []")

    if west_projects:
        lines.append("")
        lines.append("    # ADR 0018 libraries not imported by Zephyr's own west.yml.")
        for lib, west in west_projects:
            lines.append(f"    - name: {west['name']}")
            lines.append(f"      url: {west['url']}")
            lines.append(f"      revision: {west['revision']}")
            lines.append(f"      path: {west['path']}        # board.yaml libraries: '{lib}'")

    if unsupported:
        lines.append("")
        lines.append("# The following libraries have no Zephyr west project entry today")
        lines.append("# (header-only/profile libraries ride the loader's include path;")
        lines.append("# Yocto-only or in-tree Zephyr subsystems do not need a project pin):")
        for lib in unsupported:
            lines.append(f"#   - {lib}")
    return "\n".join(lines) + "\n"
