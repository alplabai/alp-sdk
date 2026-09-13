#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate covering two related silent-overlay-drop defects:

1. (issue #1009) If an example ships >=1 board-qualified overlay/conf
   under `boards/`, then every core its `board.yaml` declares via
   `cores.<core>.app:` must have its OWN matching qualified overlay
   there too -- a declared core with no matching file, while qualified
   overlays exist for other core(s), means the core the app actually
   builds silently loses its overlay.

2. (issue #2101) If an example's `testcase.yaml` ships >=1
   `alp_e1m_*`-qualified `.overlay` under `boards/`, then every
   `alp_e1m_*` `platform_allow` entry across its scenarios must have its
   own matching `.overlay` too -- same silent-drop shape, keyed off
   `platform_allow` instead of `board.yaml` so it also covers apps
   (`*-regcheck` bench checks) that have no `board.yaml` at all.

Why this exists (issue #1009): `examples/aen/edgeai-vision-aen` declared
`cores.m55_hp.app:` (and its `CMakeLists.txt` builds with `--core
m55_hp`), but shipped only the HE-qualified overlay filename
(`alp_e1m_aen801_m55_he_..._rtss_he.overlay`) under `boards/`. Zephyr
auto-applies a board overlay by matching the fully-qualified board
name, so building for `m55_hp` silently found no overlay for `m55_hp`
and dropped it -- including the ITCM retarget it carried, so the image
linked for MRAM instead. Nothing caught this: it surfaced only when a
bench operator had to hand-supply the retarget.

`check_board_target_tree_parity.py` does NOT cover this gap -- it
validates SoM-preset `topology.<core>.board:` targets against real
`zephyr/boards/alp/` trees (the board *tree* layer), not a per-example
overlay filename (the example *slice* layer).

Rule
----
Applies only to examples that ship a `board.yaml` (a plain Zephyr app
with no `board.yaml` -- e.g. `examples/connectivity/firmware-update-log`,
which picks its per-core retarget with a `BOARD MATCHES` guard in
`CMakeLists.txt`, a `*-regcheck` bench app, or a multi-slice example
whose per-core sub-directory owns its own overlays -- is out of scope;
it never goes
through `alp_project.py`'s per-core `board.yaml` -> qualified-target
resolution this gate is checking).

For an example with a `board.yaml`:

  1. Resolve its SoM preset (`som.sku` -> `metadata/e1m_modules/<sku>.yaml`)
     and read every `topology.<core>.board:` target that preset declares
     (a Zephyr-buildable core; a Yocto core like `a32_cluster` /
     `a55_cluster` has no `board:` key and is out of scope -- it has no
     overlay to check).  Each target's qualifier path (`vendor/soc/soc_id/
     variant/cpucluster`) is converted to the filename stem Zephyr's
     auto-apply uses by replacing every `/` with `_` (the same
     transform the shipped trees follow, e.g. `alp_e1m_aen801_m55_hp/
     ae822fa0e5597ls0/rtss_hp` -> `alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp`).

  2. Declared cores = every `cores.<core>` in `board.yaml` that carries
     an `app:` key -- the cores this example's own CMakeLists.txt
     actually builds (`alp_project.py --core <core>`).

  3. Walk `boards/*` (any extension).  A file is "board-qualified" iff
     its stem exactly matches one of the SoM's topology stems computed
     in (1) -- this deliberately excludes `native_sim_*` files and any
     other filename that isn't a real qualified board target, so extra
     per-scenario `.conf` variants (e.g. `..._firewall_probe.conf`)
     never false-positive.

  4. If the example ships >=1 board-qualified file at all, every
     declared core that has a topology `board:` target must have a
     matching file in `boards/`. A declared core with no matching
     file, while board-qualified files exist for other core(s), is the
     issue #1009 defect: the overlay for the declared core was never
     shipped (or was shipped under the wrong core's filename), so
     Zephyr's auto-apply-by-board-name silently finds nothing for the
     core the app is actually built for.

     Deliberately NOT checked (the false-positive this gate must
     avoid): a board-qualified file present for a core the example
     does NOT declare via `cores.<core>.app`. `examples/peripheral-io/
     blink` ships both the HE and HP overlay under `boards/` while its
     `board.yaml` declares only `cores.m55_hp.app` -- its own comment
     explains the HE overlay is kept for advanced users who build the
     peer core directly with `west build -b <he-target>`, bypassing
     `board.yaml` entirely. That extra file is inert (an unreferenced
     overlay Zephyr never auto-applies to THIS app's own declared
     build), not a silent-drop hazard, so flagging it would be a false
     positive against a real, intentional shape -- unlike a *missing*
     overlay for a core the app does build, which silently degrades
     the actual build.

Run locally:

    python3 scripts/check_example_board_overlay_parity.py

CI wires this in `pr-metadata-validate.yml`.

Extension (issue #2101): platform_allow vs boards/ parity
-----------------------------------------------------------
A second, independent check in this same script covers the sibling defect
issue #2101 describes: an example's `testcase.yaml` can declare a
`platform_allow` entry for a real E1M SoM board target for which the
example ships no matching overlay at all, while shipping one for a
*different* E1M target in the very same file. Zephyr's board-overlay
auto-apply silently finds nothing for the un-covered target and the build
proceeds with no error and no warning -- unlike the first check above,
this one does not require a `board.yaml` (`*-regcheck` bench apps like
`examples/aen/aen-adc-regcheck` have a `testcase.yaml` but no `board.yaml`,
and are exactly the shape #2101 is about).

Measured on the AEN803 board-tree branch (`feat/2084-aen803-board-tree`):
82 examples ship an AEN801-qualified overlay under `boards/`; only 2 ship
an AEN803 one, and only one example's `testcase.yaml` even lists an
AEN803 `platform_allow` entry -- so building any of the other ~80 for
`alp_e1m_aen803_m55_he/...` silently drops the overlay entirely.

Filename-stem transform (the "right key" this check compares against) --
confirmed by reading Zephyr's own CMake board-resolution code in a real
`zephyr` v4.4 checkout, not inferred from this repo's filenames:

  - `cmake/modules/boards.cmake:300`:
    `string(REPLACE "/" "_" NORMALIZED_BOARD_TARGET "${BOARD}/${BOARD_QUALIFIERS}")`
    -- the canonical underscored form of a fully-qualified board target.
  - An app's own `boards/` subdirectory is what actually gets scanned for
    a matching `.overlay`: `cmake/modules/configuration_files.cmake:72`
    calls `zephyr_file(CONF_FILES ${APPLICATION_CONFIG_DIR}/boards DTS
    DTC_OVERLAY_FILE ...)`, which (`cmake/modules/extensions.cmake`,
    `zephyr_file()` CONF_FILES mode, ~line 2892) builds its candidate
    filename via `zephyr_build_string()`:
      `string(REPLACE "/" ";" str_segment_list "${BOARD_QUALIFIERS}")`
      `string(JOIN "_" ${outvar} ${BOARD} ${str_segment_list} ${revision})`
    then appends `.overlay` (`extensions.cmake:2909`,
    `list(TRANSFORM dts_filename_list APPEND ".overlay")`).
  Both sites reduce to the same transform this check applies: join the
  board name and every `/`-separated qualifier segment with `_`, i.e.
  replace every `/` in the qualified target string with `_`.

Scope, and why (avoiding the "worse than no check" false-positive trap):
this check only compares `platform_allow` entries that name a real E1M
SoM board target (`alp_e1m_*`). A full sweep of `origin/dev` shows ~40
examples whose only `boards/` content is a `native_sim_*` overlay/conf
(native_sim frequently needs one to stub a simulated DT node that has no
physical counterpart) alongside a `platform_allow` entry for a vendor
reference board (e.g. `ensemble_e8_dk`) that ships no overlay of its own
-- on purpose, those examples never touch alp-owned board customization
at all. Requiring an overlay there would be exactly the false-positive
the issue warns against. Scoping to `alp_e1m_*` targets keeps the check
to the class #2101 actually describes: two SKUs of the *same* SoM family
declared in the *same* `testcase.yaml`, one with an overlay and one
without.

Within that `alp_e1m_*` scope: if an example's `boards/` ships zero
`alp_e1m_*`-qualified `.overlay` files at all, it is legitimately
board-agnostic for that family (e.g. `examples/ai/cold-chain-monitor`
lists an `alp_e1m_aen801_m55_hp` `platform_allow` entry for a bench
build_only scenario but ships only a `native_sim` overlay -- nothing to
compare against, so it is not flagged). Only once >=1 `alp_e1m_*`
overlay exists does every other `alp_e1m_*` `platform_allow` entry in
that same file need its own matching one.

On `origin/dev` today (no AEN803 board tree yet) this finds 0 problems --
expected, since #2101 is only reachable once a second SKU's
`platform_allow` entries land. It exists so that landing is loud instead
of silent.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
_SOM_BOARD_PREFIX = "alp_e1m_"


def _topology_stems(preset_path: Path) -> dict[str, str]:
    """core -> qualified board filename stem, for every core in the SoM
    preset's `topology:` that declares a `board:` target (Zephyr-
    buildable cores only; Yocto cores have no `board:` key)."""
    with preset_path.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    topology = doc.get("topology") or {}
    stems: dict[str, str] = {}
    if not isinstance(topology, dict):
        return stems
    for core, entry in topology.items():
        if not isinstance(entry, dict) or "board" not in entry:
            continue
        raw = str(entry["board"]).strip().split()[0]
        stems[core] = raw.replace("/", "_")
    return stems


def _declared_app_cores(board_yaml: Path) -> set[str]:
    with board_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    cores = doc.get("cores") or {}
    if not isinstance(cores, dict):
        return set()
    return {
        core for core, entry in cores.items()
        if isinstance(entry, dict) and "app" in entry
    }


def _declared_core_overlay_problems(root: Path) -> list[str]:
    """Issue #1009 class: a `board.yaml`-driven example's declared core has
    no matching `boards/` overlay while another declared core does."""
    problems: list[str] = []
    presets_dir = root / "metadata" / "e1m_modules"
    examples_dir = root / "examples"
    if not examples_dir.is_dir() or not presets_dir.is_dir():
        return problems

    for board_yaml in sorted(examples_dir.glob("*/*/board.yaml")):
        example_dir = board_yaml.parent
        rel = example_dir.relative_to(root)

        with board_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        sku = (doc.get("som") or {}).get("sku")
        if not sku:
            continue
        preset_path = presets_dir / f"{sku}.yaml"
        if not preset_path.is_file():
            continue

        stems = _topology_stems(preset_path)
        if not stems:
            continue
        stem_to_core = stems  # core -> stem
        core_by_stem = {v: k for k, v in stems.items()}

        declared = _declared_app_cores(board_yaml)

        boards_dir = example_dir / "boards"
        if not boards_dir.is_dir():
            continue

        qualified_present: dict[str, str] = {}  # core -> filename found
        for f in sorted(boards_dir.iterdir()):
            if not f.is_file():
                continue
            core = core_by_stem.get(f.stem)
            if core is None:
                continue
            qualified_present[core] = f.name

        if not qualified_present:
            continue

        for core in sorted(declared):
            if core not in stem_to_core:
                continue  # declared core has no Zephyr board target (Yocto)
            if core in qualified_present:
                continue
            others = sorted(qualified_present.values())
            problems.append(
                f"{rel}: cores.{core}.app is declared ({sku} board target "
                f"'{stem_to_core[core]}') but boards/{stem_to_core[core]}.* "
                f"is missing, while boards/ ships qualified overlay(s) for "
                f"other core(s) ({', '.join(others)}) -- the overlay for "
                f"'{core}' was silently dropped (issue #1009 class)"
            )

    return problems


def _qualified_target_to_stem(target: str) -> str:
    """Fully-qualified Zephyr board target -> the overlay filename stem
    Zephyr's own CMake resolves against an app's `boards/` directory.  See
    the module docstring for the citation in Zephyr's CMake sources."""
    return target.strip().replace("/", "_")


def _platform_allow_entries(testcase_yaml: Path) -> set[str]:
    with testcase_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}
    tests = doc.get("tests") or {}
    entries: set[str] = set()
    if not isinstance(tests, dict):
        return entries
    for scenario in tests.values():
        if not isinstance(scenario, dict):
            continue
        allow = scenario.get("platform_allow") or []
        if isinstance(allow, str):
            allow = allow.split()
        entries.update(str(e) for e in allow)
    return entries


def _platform_allow_overlay_problems(root: Path) -> list[str]:
    """Issue #2101 class: a `testcase.yaml` `platform_allow` entry for a
    real E1M SoM board target (`alp_e1m_*`) has no matching `boards/`
    overlay while a *different* `alp_e1m_*` target in the same file does.
    Unlike `_declared_core_overlay_problems`, this needs no `board.yaml` --
    it covers `*-regcheck` bench apps too."""
    problems: list[str] = []
    examples_dir = root / "examples"
    if not examples_dir.is_dir():
        return problems

    for testcase_yaml in sorted(examples_dir.glob("*/*/testcase.yaml")):
        example_dir = testcase_yaml.parent
        rel = example_dir.relative_to(root)

        som_entries = {
            e for e in _platform_allow_entries(testcase_yaml)
            if e.startswith(_SOM_BOARD_PREFIX)
        }
        if not som_entries:
            continue

        boards_dir = example_dir / "boards"
        if not boards_dir.is_dir():
            continue

        overlay_stems = {
            f.stem for f in boards_dir.iterdir()
            if f.is_file() and f.suffix == ".overlay"
        }
        entry_stems = {e: _qualified_target_to_stem(e) for e in som_entries}
        present = {e: s for e, s in entry_stems.items() if s in overlay_stems}
        if not present:
            # Zero alp_e1m_*-qualified overlays at all: this example is
            # legitimately board-agnostic for this SoM family (e.g. a
            # native_sim-only boards/ dir). Nothing to compare against.
            continue

        others = sorted(present.values())
        for entry in sorted(som_entries):
            if entry in present:
                continue
            stem = entry_stems[entry]
            problems.append(
                f"{rel}: testcase.yaml platform_allow entry '{entry}' has "
                f"no matching boards/{stem}.overlay, while boards/ ships a "
                f"qualified overlay for other alp_e1m_* target(s) in the "
                f"same testcase.yaml ({', '.join(others)}) -- the build "
                f"for '{entry}' silently drops its board overlay "
                f"(issue #2101 class)"
            )

    return problems


def find_problems(root: Path) -> list[str]:
    problems = _declared_core_overlay_problems(root)
    problems.extend(_platform_allow_overlay_problems(root))
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_example_board_overlay_parity: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every example board.yaml core with an app: key, and every "
          "testcase.yaml alp_e1m_* platform_allow entry, has a matching "
          "boards/ overlay wherever the example ships qualified overlays.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
