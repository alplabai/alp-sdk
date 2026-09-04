#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove every `libraries:`-selectable Zephyr module actually resolves in
west.yml -- the alp-sdk-OWNED workspace (`west init -l alp-sdk`, what `tan
bootstrap` and every customer runs), not the Zephyr-owned one pr-twister.yml
happens to init from (`west init -m zephyr`) instead.

#1136: nanopb was reachable in CI (pr-twister.yml's `west init -m zephyr`
topdir resolves it via Zephyr's own unconditioned project) while completely
unreachable in a real alp-sdk-owned workspace -- `metadata/libraries/
nanopb.yaml` names west module `nanopb`, but west.yml's Zephyr import
`name-allowlist` didn't list it, and examples/connectivity/nanopb-encode-
decode/CMakeLists.txt FATAL_ERRORs with no fallback when
ZEPHYR_NANOPB_MODULE_DIR is unset. Nothing caught this because the gate that
DOES resolve alp-sdk's own manifest doesn't exist -- until now.

A follow-up defect the same fix uncovered: west's project-name resolution
lets a top-level `projects:` entry silently SUPPRESS a same-named
`name-allowlist` import, even while sitting in a permanently-disabled group
-- so an allowlist entry can look correct in the diff and still be dead
weight in every possible `--group-filter` (#650 hit exactly this, then
reverted the entry it had correctly added, because at the time
`nanopb-mproc-pin` below was still just named `nanopb`).

Three structural checks (1 and 2 below; check 3 follows after), all static
(no `west update`, no network -- the
manifest text is the single source of truth for what's *reachable*; whether
a customer's group-filter currently *enables* an optional/tier1/vendor-sdks
group is a deliberate, documented, per-library opt-in this gate does not
second-guess -- jsmn, Catch2, u8g2, BearSSL, etl, fmt, doctest,
libwebsockets, libmodbus, madgwick_ahrs, minimp3, and opus all
sit ONLY inside a disabled-by-default group today, entirely by design; see
the `group-filter:` comment block in west.yml. A top-level project counts as
reachable here regardless of its own `groups:` state -- the defect this
gate polices is a module with NO resolution path at all, not one gated
behind a documented opt-in flip):

  1. No west.yml top-level `projects:` entry shares a `name:` with a
     `name-allowlist` entry under the `zephyr` project's `import:` block.
     A collision means the top-level entry ALWAYS wins (west's own
     resolution rule), so the allowlist entry can never actually activate --
     regardless of group-filter state.
  2. Every `metadata/libraries/*.yaml` naming a real Zephyr module
     (`integration.zephyr.module`, non-null) resolves to SOME west.yml
     project -- either a top-level `projects:` entry of that name, or a
     `name-allowlist` entry (and not shadowed per check 1). A library
     manifest whose `integration.zephyr.west:` block supplies its own
     standalone west pin (the ADR 0018 Tier-B `--emit west-libraries` path
     -- e.g. azure-iot, aws-iot, micropython) is exempt: that mechanism
     synthesizes its own west project entry at emit time and never touches
     west.yml directly.

A third, narrower check adds back exactly the precision check 2 gives up:
for a `tier: A` library only (ADR 0018 -- curated, bundled, no group opt-in
promised to the customer), the resolving west.yml project must ALSO not be
disabled by this manifest's own top-level `group-filter:` (a top-level
project whose every `groups:` entry is disabled does not count). This is
scoped to tier A specifically because it is empirically the dividing line in
this repo today: every tier-A library (nanopb, cmsis-nn, cmsis-dsp, zcbor,
lvgl; modbus declares no module) already resolves unconditionally, while
every module gated behind a disabled-by-default group is tier B. Applying
this stricter rule to check 2's tier-B libraries as well would misfire on
all thirteen of them for a documented, working opt-in, not a defect --
that's exactly the false-positive check 3 exists to avoid by staying tier-A
only.

KNOWN LIMIT: neither check 2 nor check 3 can verify a `name-allowlist`
entry's upstream group inside *Zephyr's own* west.yml (the
tflite-micro/`+optional` shape, #1076) -- that needs a real Zephyr checkout
to read, which is exactly the network-heavy oracle this gate deliberately
avoids so it runs on every PR for free. A `name-allowlist` entry always
counts as reachable (and group-unconditioned) here regardless of its
upstream group state.

Run locally:

    python3 scripts/check_west_manifest_module_resolution.py
"""
from __future__ import annotations

import sys
from pathlib import Path

import yaml

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent


def _load_west_manifest(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8"))["manifest"]


def _top_level_project_names(manifest: dict) -> set[str]:
    return {p["name"] for p in manifest.get("projects", []) if "name" in p}


def _disabled_groups(manifest: dict) -> set[str]:
    """Group names disabled by this manifest's own top-level `group-filter:`
    -- the bare `-<group>` entries. `+<group>` re-enables are not modelled
    (none are in use for top-level projects today)."""
    return {
        entry[1:]
        for entry in manifest.get("group-filter", []) or []
        if isinstance(entry, str) and entry.startswith("-")
    }


def _default_active_top_level_project_names(manifest: dict) -> set[str]:
    """Top-level project names reachable under this manifest's OWN default
    `group-filter:` -- excludes a project whose every declared `groups:`
    entry is disabled. A project with no `groups:` at all is always
    active. Used only by check 3 (tier-A libraries); check 2 deliberately
    stays group-blind for everything else (see module docstring)."""
    disabled = _disabled_groups(manifest)
    active = set()
    for p in manifest.get("projects", []):
        name = p.get("name")
        if not name:
            continue
        groups = p.get("groups")
        if not groups or not set(groups) <= disabled:
            active.add(name)
    return active


def _zephyr_name_allowlist(manifest: dict) -> set[str]:
    for p in manifest.get("projects", []):
        if p.get("name") == "zephyr":
            imp = p.get("import")
            if isinstance(imp, dict):
                return set(imp.get("name-allowlist", []))
    return set()


def _check_no_shadowed_allowlist_entries(top_level: set[str], allowlist: set[str]) -> list[str]:
    problems = []
    for name in sorted(top_level & allowlist):
        problems.append(
            f"west.yml top-level project '{name}' shares its name with the zephyr "
            f"import's name-allowlist entry '{name}' -- west's project-name dedup "
            f"lets the directly-listed project suppress the imported one "
            f"UNCONDITIONALLY (even from a disabled group), so the allowlist entry "
            f"can never activate in any group-filter configuration; rename one of "
            f"them (#1136)"
        )
    return problems


def _library_module_requirements(libdir: Path) -> list[tuple[str, str, str]]:
    """(library name, required west module name, tier) for every
    metadata/libraries/*.yaml naming a real Zephyr module with no
    self-contained `west:` pin."""
    out: list[tuple[str, str, str]] = []
    if not libdir.is_dir():
        return out
    for path in sorted(libdir.glob("*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        zephyr = (doc.get("integration") or {}).get("zephyr")
        if not isinstance(zephyr, dict):
            continue
        module = zephyr.get("module")
        if not module:
            continue
        if zephyr.get("west") is not None:
            # ADR 0018 Tier-B: the library manifest carries its own standalone
            # west pin, resolved by `--emit west-libraries` at generation
            # time -- never a static west.yml entry to look for.
            continue
        out.append((doc.get("name", path.stem), module, doc.get("tier", "")))
    return out


def _check_every_module_reachable(
    requirements: list[tuple[str, str, str]], top_level: set[str], allowlist: set[str]
) -> list[str]:
    reachable = top_level | allowlist
    problems = []
    for lib_name, module, _tier in requirements:
        if module not in reachable:
            problems.append(
                f"metadata/libraries/{lib_name}.yaml declares "
                f"integration.zephyr.module: '{module}', but west.yml defines no "
                f"top-level project of that name and does not allowlist it under "
                f"the zephyr import -- `west update` in an alp-sdk-owned workspace "
                f"(`west init -l alp-sdk`) can never fetch it, in ANY "
                f"group-filter configuration (#1136 shape)"
            )
    return problems


def _check_tier_a_modules_are_group_unconditioned(
    requirements: list[tuple[str, str, str]],
    default_active_top_level: set[str],
    allowlist: set[str],
) -> list[str]:
    """A `tier: A` library (ADR 0018 -- curated, bundled, no group opt-in
    promised to the customer) must resolve without any `group-filter` flip.
    Skips a module already flagged unreachable by check 2 -- one problem per
    real gap, not two for the same module."""
    reachable = default_active_top_level | allowlist
    problems = []
    for lib_name, module, tier in requirements:
        if tier != "A" or module in reachable:
            continue
        problems.append(
            f"metadata/libraries/{lib_name}.yaml is tier: A (ADR 0018 -- "
            f"must resolve with no group-filter opt-in), but its west module "
            f"'{module}' is reachable only through a west.yml top-level "
            f"project whose `groups:` are all disabled by this manifest's "
            f"own default `group-filter:` -- a real customer's default "
            f"`west update` can never fetch it (#1136 shape: this is "
            f"exactly how the original `nanopb` top-level pin masked the "
            f"missing allowlist entry)"
        )
    return problems


def find_problems(root: Path) -> list[str]:
    west_yml = root / "west.yml"
    libdir = root / "metadata" / "libraries"
    if not west_yml.is_file():
        return [f"missing {west_yml}"]
    manifest = _load_west_manifest(west_yml)
    top_level = _top_level_project_names(manifest)
    default_active_top_level = _default_active_top_level_project_names(manifest)
    allowlist = _zephyr_name_allowlist(manifest)

    problems = _check_no_shadowed_allowlist_entries(top_level, allowlist)
    requirements = _library_module_requirements(libdir)
    problems += _check_every_module_reachable(requirements, top_level, allowlist)
    problems += _check_tier_a_modules_are_group_unconditioned(
        requirements, default_active_top_level, allowlist
    )
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"west-manifest-module-resolution: {p}", file=sys.stderr)
        return 1
    print("OK: every west.yml name-allowlist entry is unshadowed, and every "
          "libraries:-selectable Zephyr module resolves to a reachable west.yml "
          "project.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
