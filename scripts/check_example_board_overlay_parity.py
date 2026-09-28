#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate covering four related silent-overlay-drop defects:

1. (issue #1009) If an example ships >=1 board-qualified overlay/conf
   under `boards/`, then every core its `board.yaml` declares via
   `cores.<core>.app:` must have its OWN matching qualified overlay
   there too -- a declared core with no matching file, while qualified
   overlays exist for other core(s), means the core the app actually
   builds silently loses its overlay.

2. (issue #2101) If an example ships >=1 `alp_e1m_*`-qualified file
   (any extension) under `boards/`, then every `alp_e1m_*`
   `platform_allow` entry across its `testcase.yaml` scenarios must have
   its own matching file too -- same silent-drop shape, keyed off
   `platform_allow` instead of `board.yaml` so it also covers apps
   (`*-regcheck` bench checks) that have no `board.yaml` at all, and off
   `boards/` *content* rather than which entries happen to be declared,
   so it also catches the case where the shipped overlay no longer
   matches anything `platform_allow` names at all.

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

Measured on the AEN803 board-tree branch (`feat/2084-aen803-board-tree`
@ `02e5e6f361f8610109aff951b7e1fa6ceae7f90c`), counting *directories*, not
files: 81 example directories under `examples/aen/`; 73 ship an
`alp_e1m_aen801*` file under `boards/`; 1 (`aen-adc-regcheck`) ships an
`alp_e1m_aen803*` file; 72 ship 801 but not 803. This check covers **0**
of those 72 -- none of them declares an AEN803 `platform_allow` entry, so
a `platform_allow`-keyed check has nothing to compare against for them.
What it DOES catch is the case where a `platform_allow` entry is actually
declared for a target with no overlay (e.g. `aen-brd-i2c-scan` with its
single `platform_allow` entry swapped from the AEN801 target to the
AEN803 one, overlay left AEN801-only -- rc=1 on that mutation, see the
test suite). Closing the other 72 needs either the overlays themselves
(out of scope here, owned by #2100/#2103) or the broader repo-state check
below.

Filename-stem transform (the "right key" this check compares against) --
confirmed by reading Zephyr's own CMake board-resolution code in a real
`zephyr` v4.4 checkout, not inferred from this repo's filenames. The one
site that actually resolves an app's own `boards/` overlay is
`cmake/modules/configuration_files.cmake:72`:
`zephyr_file(CONF_FILES ${APPLICATION_CONFIG_DIR}/boards DTS
DTC_OVERLAY_FILE ...)`. That macro (`cmake/modules/extensions.cmake`,
`zephyr_file()` CONF_FILES mode, ~line 2892-2916) builds its candidate
filename(s) via `zephyr_build_string(... MERGE REVERSE ... SHORT ...)`
(`extensions.cmake:1684`):
  `string(REPLACE "/" ";" str_segment_list "${BUILD_STR_BOARD_QUALIFIERS}")`
  (`:1717`)
  `string(JOIN "_" ${outvar} ${BUILD_STR_BOARD} ${str_segment_list} ${revision_string})`
  (`:1719`)
then appends `.overlay` (`extensions.cmake:2909`,
`list(TRANSFORM dts_filename_list APPEND ".overlay")`). Because that call
passes both `MERGE REVERSE` and `SHORT`, it produces TWO candidate
stems, and `zephyr_file()` accepts either (`test_file_0 OR test_file_1`,
`:2934`): the full stem (board + every qualifier segment) and a SHORT
stem that drops the first qualifier segment (the SoC id). Verified with
`cmake -P` against a real qualified target: Zephyr accepts both
`alp_e1m_aen803_m55_he_ae822fa0e5597ls0_rtss_he` and the SHORT form
`alp_e1m_aen803_m55_he_rtss_he`. This check accepts either stem too --
requiring only the full one would false-positive on a real, Zephyr-valid
SHORT-form overlay. (`cmake/modules/boards.cmake:300`'s
`NORMALIZED_BOARD_TARGET` uses the identical join-with-`_` transform but
is a red herring as a second confirmation: it is consumed only by the
Xtensa xcc toolchain file and has nothing to do with `boards/`
resolution -- `configuration_files.cmake:72` is the only real site.)

Scope, and why (avoiding the "worse than no check" false-positive trap):
this check only compares `platform_allow` entries that name a real E1M
SoM board target (`alp_e1m_*`). A full sweep of `origin/dev` shows ~40
examples whose only `boards/` content is a `native_sim_*` overlay/conf
(native_sim frequently needs one to stub a simulated DT node that has no
physical counterpart) alongside a `platform_allow` entry for a vendor
reference board (e.g. `ensemble_e8_dk`) that ships no overlay of its own.
For most of those, that is legitimate: they never touch alp-owned board
customization. It is NOT universally true, though --
`examples/peripheral-io/drone-autopilot` ships an `alp_e1m_aen801_m55_hp`
overlay (carrying the `alp-i2c0` / `alp-uart1` / `alp-pwm0..3` aliases
its `src/autopilot.c` opens) but its only hardware `platform_allow`
entry is `ensemble_e8_dk/ae822fa0e5597ls0/rtss_hp`, which has no matching
overlay of its own -- the CI build it actually runs silently drops every
one of those aliases. This check's `alp_e1m_*` scoping does not (and, by
construction, cannot) catch that instance, because `ensemble_e8_dk` is
not an `alp_e1m_*` target; it is reported separately rather than folded
into this fix. Scoping to `alp_e1m_*` targets keeps this check to the
narrower class #2101 actually describes and can be mechanically proven
about: two SKUs of the *same* SoM family declared in the *same*
`testcase.yaml`, one with an overlay and one without.

Within that `alp_e1m_*` scope: the precondition for "this example
actually uses per-target overlays for this family" is board-content-
based, not declaration-based -- it is "`boards/` ships >=1 `alp_e1m_*`-
qualified overlay", checked directly against the files on disk, NOT
against whether any *declared* `platform_allow` entry happens to match
one. Keying it off the declared entries instead would miss the class
#2101 was filed over outright: `aen-brd-i2c-scan` on the AEN803 branch
ships an AEN801 overlay under `boards/` but declares only the AEN803
target in `platform_allow` -- an example that is board-agnostic by the
wrong (declaration-based) precondition, when its `boards/` directory
proves it is not. If an example's `boards/` ships zero `alp_e1m_*`-
qualified overlays at all, it IS legitimately board-agnostic for that
family (e.g. `examples/ai/cold-chain-monitor` lists an
`alp_e1m_aen801_m55_hp` `platform_allow` entry for a bench build_only
scenario but ships only a `native_sim` overlay -- nothing on disk to
compare against, so it is not flagged). Only once >=1 `alp_e1m_*`
overlay exists ON DISK does every `alp_e1m_*` `platform_allow` entry in
that same file need its own matching one.

On `origin/dev` today this finds 0 problems -- but that means "nobody
has declared a second SKU's `platform_allow` entry yet", not "this class
is absent from the tree". It is reachable today, before AEN803 lands:
`zephyr/boards/alp/` already ships `e1m_aen401_m55_hp` and
`e1m_aen601_m55_hp` board trees (both `status: preliminary: true`,
`partial_hw_config: true` in their `metadata/e1m_modules/*.yaml`), and 12
examples ship an `alp_e1m_aen801_m55_hp`-qualified overlay with no
AEN401/AEN601 sibling. Building any of those 12 for
`alp_e1m_aen601_m55_hp/ae612fa0e5597ls0/rtss_hp` would drop the overlay
silently today -- this check does not catch it either, for the same
declaration-scoping reason: none of those 12 declares an AEN401/AEN601
`platform_allow` entry. The broader, repo-state-keyed check below would.

A broader, still-deferred alternative: instead of keying off what a
`testcase.yaml` happens to *declare*, key off `boards/` content plus
`zephyr/boards/alp/` tree membership directly -- for every
`boards/<alp_e1m_SKUA>_<quals>.overlay` an example ships, if
`zephyr/boards/alp/` also ships a sibling SKU's board tree whose
`metadata/e1m_modules/<SKU>.yaml` `topology.<core>.board:` names the
same core, require the sibling's overlay too (or an explicit allow-list
entry) -- with NO dependence on any `platform_allow` entry existing at
all. This is fully computable from committed metadata and would cover
all 72 AEN801-but-not-AEN803 examples above plus the AEN401/AEN601 gap.
It is deliberately NOT implemented here: it is red today (the 12 HP/
AEN401/AEN601 examples above), and landing it would need either those
overlays or an allow-list alongside it -- both larger, separate changes.
Filed as a follow-up rather than silently deferred. "Extension (issue
#2209)" below implements a narrower, declaration-triggered relative of
this idea -- see that section for how it differs and why it still does
NOT close the AEN401/AEN601/HP gap described here.

Extension (issue #2207): a boards/ directory with no app
---------------------------------------------------------
A third, independent check: every `examples/**/boards/` directory's parent
must hold a `CMakeLists.txt`. A `boards/` directory with no app next to it
is built by nothing, so both checks above -- and
`check_example_board_overlay_content_parity.py`, which only compares a file
with its SKU sibling -- walk straight past it. #2198 found one on `dev`:
`00bcd9f2f` (#2122) renamed `examples/aen/aen-sdcard-readout` to
`aen-sdhc-probe` while `df628b6ab` (#2176) added an AEN803 overlay at the old
path, and both merged -- leaving that overlay, with `sdhc0` still enabled
against the 2626-R2 SDIO mux #2051 disabled it for, alone in a directory with
no app. Nothing breaks while it sits there; anyone who restores or copies the
app inherits it. It is the same silently-dropped-overlay class as #1009 and
#2101, reached by a rename instead of a missing file, which is why it lives
here. No allowlist: the fix is always to delete the directory or move it to
where the app now lives.

Only git-TRACKED files count, so a local `west build` inside an example (its
gitignored `build/zephyr/boards/`, `build/Kconfig/boards/`) is not reported.
`<app>/sysbuild/<image>/boards/` is accepted when `<app>/CMakeLists.txt`
exists: that is sysbuild's per-image configuration directory
(`zephyr/share/sysbuild/cmake/modules/sysbuild_extensions.cmake`, the
`${APP_DIR}/sysbuild/${ZBUILD_APPLICATION}` lookup). Outside a git checkout
(e.g. a `git archive` tree) every file on disk counts.

Extension (issue #2209): same-PCB sibling undeclared
------------------------------------------------------
A fourth, independent check -- narrower than the "broader, deferred
alternative" filed as a follow-up when #2101 landed (see that note above;
this is NOT that check). A scenario's `platform_allow` names a real
`alp_e1m_*` SKU A core target; `boards/` ships a qualified overlay for that
same core under a SIBLING SKU B, where A and B are the SAME PCB (their
`metadata/e1m_modules/E1M-<SKU>.yaml` declare the same `family` AND the same
`silicon_variant` -- the identical pairing
`check_example_board_overlay_content_parity.py`'s `_pcb_key` uses); B's
target is not declared anywhere this scenario's own build can reach. Like
`_platform_allow_overlay_problems` above, this still requires a real
`platform_allow` DECLARATION to trigger (SKU A's), and both overlays to
already exist on disk -- it does NOT fire from `boards/` content alone with
zero declarations, so it does NOT close the AEN401/AEN601/HP gap the
deferred-alternative note above describes. What it DOES catch, that the
#2101 check above does not: the ~50 scenarios #2209 declared the AEN803
target for (each named only the AEN801 target while `boards/` already
shipped the same-PCB AEN803 overlay for the same core) -- because this
check's trigger is `boards/` CONTENT plus SoM-preset pairing for the
DECLARED SKU, not "some declared entry already matches something on disk".

Only `extra_dtc_overlay_files`, or an `extra_args` entry starting
`DTC_OVERLAY_FILE=` or `CONF_FILE=`, REPLACES (rather than supplements)
Zephyr's own board-default-overlay auto-discovery (verified against
`zephyr/cmake/modules/configuration_files.cmake`'s `if(NOT DEFINED
DTC_OVERLAY_FILE)` guard and `config_parser.py`'s `extract_fields_from_arg_list`
pull-out of those two CMake vars from `extra_args`); `extra_conf_files` and
`extra_overlay_confs` only ADD files and never suppress this check. A
scenario pinned this way is deliberately single-SKU by design -- the
`aen-sdhc-probe`/`aen-ethernet-link` twin scenarios #2209 added, each
pinning one SKU's file explicitly and giving the other SKU its OWN twin
scenario rather than a second `platform_allow` entry on the same one -- so
such a scenario is not required to declare its OWN sibling target; instead,
some OTHER pinned scenario in the same `testcase.yaml` must declare the
sibling SKU's target for the same core (the twin). Deleting that twin, with
nothing else in the file pinning the sibling's own file, fails this check.
A non-pinned scenario gets no such fallback: it must declare its own
sibling target itself, in the same scenario.

`platform_allow` is read as Twister actually resolves it: a `set`-type
field, so a scenario-level value MERGES with (not replaces) the file's
`common: platform_allow:` default (`config_parser.py`'s per-scenario merge,
~lines 176-199) -- not "scenario replaces common" as an earlier version of
this note claimed.
"""
from __future__ import annotations

import argparse
import re
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


def _qualified_target_to_stems(target: str) -> list[str]:
    """Fully-qualified Zephyr board target -> the overlay filename stem(s)
    Zephyr's own CMake will accept from an app's `boards/` directory: the
    full stem (board + every qualifier segment) and, since
    `zephyr_file(CONF_FILES ...)` also tries a SHORT form that drops the
    first qualifier segment (the SoC id) and accepts either, that SHORT
    stem too. See the module docstring for the citation in Zephyr's CMake
    sources. Order: full stem first, then the short stem if it differs."""
    parts = target.strip().split("/")
    board, quals = parts[0], parts[1:]
    full = "_".join([board, *quals])
    stems = [full]
    if quals:
        short = "_".join([board, *quals[1:]]) if quals[1:] else board
        if short != full:
            stems.append(short)
    return stems


def _as_list(value):
    if value is None:
        return None
    if isinstance(value, str):
        return value.split()
    return list(value)


def _platform_allow_entries(testcase_yaml: Path) -> set[str]:
    """Every `platform_allow` entry across a testcase.yaml's scenarios --
    a scenario's own `platform_allow:` MERGED with the file's top-level
    `common: platform_allow:` default when both are present (Twister
    semantics: `platform_allow` is a `set`-type field --
    `config_parser.py`'s `testsuite_valid_keys["platform_allow"]` --  and
    its per-scenario merge, ~`config_parser.py:176-199`, concatenates a
    `set`/`list`-type common value with the scenario's own rather than
    letting the scenario replace it outright); when a scenario declares no
    `platform_allow` of its own, the common default applies alone."""
    with testcase_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}

    common = doc.get("common") or {}
    common_allow = (_as_list(common.get("platform_allow")) or []) if isinstance(common, dict) else []

    tests = doc.get("tests") or {}
    entries: set[str] = set()
    if not isinstance(tests, dict):
        return entries
    for scenario in tests.values():
        if not isinstance(scenario, dict):
            continue
        own = _as_list(scenario.get("platform_allow"))
        allow = common_allow + own if own is not None else common_allow
        entries.update(str(e) for e in allow)
    return entries


def _platform_allow_overlay_problems(root: Path) -> list[str]:
    """Issue #2101 class: a `testcase.yaml` `platform_allow` entry for a
    real E1M SoM board target (`alp_e1m_*`) has no matching `boards/`
    overlay while `boards/` ships an `alp_e1m_*`-qualified overlay for a
    *different* target. Unlike `_declared_core_overlay_problems`, this
    needs no `board.yaml` -- it covers `*-regcheck` bench apps too, and
    (via `**/testcase.yaml`) multi-slice sub-directory apps."""
    problems: list[str] = []
    examples_dir = root / "examples"
    if not examples_dir.is_dir():
        return problems

    for testcase_yaml in sorted(examples_dir.rglob("testcase.yaml")):
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

        present_stems = {f.stem for f in boards_dir.iterdir() if f.is_file()}

        # Precondition is board CONTENT, not declaration: does boards/
        # actually ship >=1 alp_e1m_*-qualified file at all, regardless
        # of whether any *declared* platform_allow entry happens to name
        # it? Keying this off "at least one declared entry has a match"
        # instead would miss exactly the #2101 defect in its fully-
        # declared form: an example whose boards/ carries the OLD SKU's
        # overlay but whose testcase.yaml now names only the NEW SKU
        # (e.g. aen-brd-i2c-scan on the AEN803 branch, platform_allow
        # swapped to alp_e1m_aen803_* with the AEN801 overlay left in
        # place) would then show zero declared-entry matches and be
        # treated as "legitimately board-agnostic" -- silent, wrong.
        som_present_stems = sorted(
            s for s in present_stems if s.startswith(_SOM_BOARD_PREFIX)
        )
        if not som_present_stems:
            # Zero alp_e1m_*-qualified files on disk at all: this example
            # is legitimately board-agnostic for this SoM family (e.g. a
            # native_sim-only boards/ dir). Nothing to compare against.
            continue

        entry_stems = {e: _qualified_target_to_stems(e) for e in som_entries}
        for entry in sorted(som_entries):
            stems = entry_stems[entry]
            if any(s in present_stems for s in stems):
                continue
            problems.append(
                f"{rel}: testcase.yaml platform_allow entry '{entry}' has "
                f"no matching boards/{stems[0]}.* overlay, while boards/ "
                f"ships alp_e1m_*-qualified overlay(s) for a different "
                f"target ({', '.join(som_present_stems)}) -- the build "
                f"for '{entry}' silently drops its board overlay "
                f"(issue #2101 class)"
            )

    return problems


def _example_files(root: Path) -> list[Path]:
    """Every tracked file under examples/ (every file, outside git)."""
    import subprocess  # lazy: only this check shells out

    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--", "examples"],
        capture_output=True, check=False)
    if proc.returncode == 0:
        return [root / f for f in proc.stdout.decode("utf-8").split("\0") if f]
    return [p for p in (root / "examples").rglob("*") if p.is_file()]


def _stranded_boards_dir_problems(root: Path) -> list[str]:
    """Issue #2207: a `boards/` directory whose parent has no CMakeLists.txt."""
    if not (root / "examples").is_dir():
        return []
    stranded: dict[Path, list[str]] = {}
    for f in _example_files(root):
        parts = f.relative_to(root).parts
        for i, part in enumerate(parts[:-1]):
            if part != "boards":
                continue
            boards = root.joinpath(*parts[:i + 1])
            app = boards.parent
            if (i >= 3 and parts[i - 2] == "sysbuild"
                    and (app.parent.parent / "CMakeLists.txt").is_file()):
                continue  # <app>/sysbuild/<image>/boards/: sysbuild's own
            if not (app / "CMakeLists.txt").is_file():
                stranded.setdefault(boards, []).append(
                    "/".join(parts[i + 1:]))
    return [
        f"{boards.relative_to(root).as_posix()}/: no CMakeLists.txt in "
        f"{boards.parent.relative_to(root).as_posix()}/, so nothing builds "
        f"these files ({', '.join(sorted(names))}) -- delete the directory, "
        f"or move it to where the app now lives (issue #2207 class)"
        for boards, names in sorted(stranded.items())
    ]


_QUALIFIED_STEM_RE = re.compile(r"^alp_e1m_([a-z0-9]+)_(.+)$")
# Only these actually REPLACE (rather than supplement) Zephyr's own
# board-default-overlay auto-discovery -- verified against
# `zephyr/cmake/modules/configuration_files.cmake`'s `if(NOT DEFINED
# DTC_OVERLAY_FILE)` guard (only `extra_dtc_overlay_files`, mapped to the
# CMake var `DTC_OVERLAY_FILE`, and an `extra_args` entry starting
# `DTC_OVERLAY_FILE=` -- `config_parser.py`'s `extract_fields_from_arg_list`
# pulls that prefix, and `CONF_FILE=`, out of `extra_args` specially -- set
# it). `extra_conf_files` (-> `EXTRA_CONF_FILE`) and `extra_overlay_confs`
# (-> `OVERLAY_CONFIG`) are separate, ADDITIVE mechanisms and never suppress
# `boards/` auto-discovery, so they must NOT count here.
_PIN_ARG_PREFIXES = ("DTC_OVERLAY_FILE=", "CONF_FILE=")


def _is_pinned(common: dict, scenario: dict) -> bool:
    """True if `scenario` (merged with the file's `common:` block, same
    additive semantics Twister uses for `extra_dtc_overlay_files`/
    `extra_args`) sets `extra_dtc_overlay_files` or an `extra_args` entry
    naming `DTC_OVERLAY_FILE=`/`CONF_FILE=` -- see `_PIN_ARG_PREFIXES`."""
    if common.get("extra_dtc_overlay_files") or scenario.get("extra_dtc_overlay_files"):
        return True
    args = (_as_list(common.get("extra_args")) or []) + (_as_list(scenario.get("extra_args")) or [])
    return any(str(a).startswith(_PIN_ARG_PREFIXES) for a in args)


def _scenario_platform_allow_and_pins(testcase_yaml: Path):
    """Yield (scenario_name, platform_allow entries, is_pinned) per
    scenario in a testcase.yaml. `platform_allow` is `common:` MERGED with
    the scenario's own (Twister's `set`-type per-scenario merge, see
    `_platform_allow_entries`'s docstring -- NOT "scenario replaces
    common"). `is_pinned` is `_is_pinned()` -- see `_PIN_ARG_PREFIXES`."""
    with testcase_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}

    common = doc.get("common") or {}
    common = common if isinstance(common, dict) else {}
    common_allow = _as_list(common.get("platform_allow")) or []

    tests = doc.get("tests") or {}
    if not isinstance(tests, dict):
        return
    for scenario_name, scenario in tests.items():
        if not isinstance(scenario, dict):
            continue
        own = _as_list(scenario.get("platform_allow"))
        allow = common_allow + own if own is not None else common_allow
        pins = _is_pinned(common, scenario)
        yield scenario_name, {str(e) for e in allow}, pins


def _all_preset_pcb_and_topology(presets_dir: Path
                                 ) -> dict[str, tuple[tuple[str, str] | None, dict[str, str]]]:
    """lowercase SKU (e.g. 'aen801') -> ((family, silicon_variant) or None,
    {core: raw fully-qualified board target}), for every
    `metadata/e1m_modules/E1M-*.yaml` preset. Read once per run, not once
    per scenario -- there are ~10 presets and potentially hundreds of
    scenarios."""
    out: dict[str, tuple[tuple[str, str] | None, dict[str, str]]] = {}
    for preset in sorted(presets_dir.glob("E1M-*.yaml")):
        sku = preset.stem[len("E1M-"):].lower()
        with preset.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        family, variant = doc.get("family"), doc.get("silicon_variant")
        pcb = (family, variant) if family and variant else None
        topology = doc.get("topology") or {}
        targets: dict[str, str] = {}
        if isinstance(topology, dict):
            for core, entry in topology.items():
                if isinstance(entry, dict) and "board" in entry:
                    targets[core] = str(entry["board"]).strip().split()[0]
        out[sku] = (pcb, targets)
    return out


def _same_pcb_sibling_undeclared_problems(root: Path) -> list[str]:
    """Issue #2209 item 4a: a scenario declares SKU A's core target while
    `boards/` ships the same-PCB SKU B's file for that same core, and B is
    undeclared anywhere this scenario's own build can reach. See the module
    docstring's "Extension (issue #2209)" section for the full rationale.

    A non-pinned scenario must declare its own sibling target itself (in
    the SAME scenario) -- a coincidental match in an unrelated scenario
    elsewhere in the file does not satisfy it. A PINNED scenario (see
    `_is_pinned`) cannot declare its own sibling this way by design (that
    shape is `check_example_board_overlay_content_parity.py`'s
    `_testcase_pin_problems` hazard -- see the docstring), so it is instead
    satisfied only by another PINNED scenario, in the same testcase.yaml,
    that itself declares the sibling SKU's target for the same core: its
    twin. Deleting a twin scenario, with nothing else in the file pinning
    the sibling SKU's own file, must fail this check -- a stray non-pinned
    scenario that happens to also declare the sibling target does not save
    it."""
    problems: list[str] = []
    examples_dir = root / "examples"
    presets_dir = root / "metadata" / "e1m_modules"
    if not examples_dir.is_dir() or not presets_dir.is_dir():
        return problems

    presets = _all_preset_pcb_and_topology(presets_dir)

    for testcase_yaml in sorted(examples_dir.rglob("testcase.yaml")):
        example_dir = testcase_yaml.parent
        rel = example_dir.relative_to(root)
        boards_dir = example_dir / "boards"
        if not boards_dir.is_dir():
            continue
        present_stems = {f.stem for f in boards_dir.iterdir() if f.is_file()}
        if not any(s.startswith(_SOM_BOARD_PREFIX) for s in present_stems):
            continue

        # Every scenario in the file, with its own declared stems, read
        # once so the pinned-twin fallback below can look sideways at
        # OTHER scenarios without re-parsing the file per scenario.
        scenarios = list(_scenario_platform_allow_and_pins(testcase_yaml))
        per_scenario_stems = [
            {s for e in allow if e.startswith(_SOM_BOARD_PREFIX)
             for s in _qualified_target_to_stems(e)}
            for _, allow, _ in scenarios
        ]

        for idx, (scenario, allow, pinned) in enumerate(scenarios):
            som_entries = {e for e in allow if e.startswith(_SOM_BOARD_PREFIX)}
            if not som_entries:
                continue
            declared_stems = per_scenario_stems[idx]

            for entry in sorted(som_entries):
                m = _QUALIFIED_STEM_RE.match(entry.replace("/", "_"))
                if not m:
                    continue
                sku_a, suffix = m.group(1), m.group(2)
                pcb_a = presets.get(sku_a, (None, {}))[0]
                if pcb_a is None:
                    continue

                for sku_b, (pcb_b, targets_b) in presets.items():
                    if sku_b == sku_a or pcb_b != pcb_a:
                        continue
                    for raw_target in targets_b.values():
                        m2 = _QUALIFIED_STEM_RE.match(raw_target.replace("/", "_"))
                        if not m2 or m2.group(2) != suffix:
                            continue
                        stems_b = _qualified_target_to_stems(raw_target)
                        if not any(s in present_stems for s in stems_b):
                            continue  # not shipped -- #2101's business, not this one
                        if any(s in declared_stems for s in stems_b):
                            continue  # this scenario already declares it itself
                        if pinned and any(
                            other_pinned and any(s in other_stems for s in stems_b)
                            for other_idx, (_, _, other_pinned) in enumerate(scenarios)
                            if other_idx != idx
                            for other_stems in [per_scenario_stems[other_idx]]
                        ):
                            continue  # a sibling PINNED twin scenario declares it
                        problems.append(
                            f"{rel}: testcase.yaml scenario {scenario!r} "
                            f"declares platform_allow entry '{entry}' (SKU "
                            f"{sku_a.upper()}) but boards/{stems_b[0]}.* "
                            f"ships the same-PCB SKU {sku_b.upper()}'s file "
                            f"for the same core, undeclared anywhere this "
                            f"scenario's own build reaches -- "
                            + (f"give {sku_b.upper()} its own twin scenario "
                               f"pinning its own file (issue #2209 item 3 shape)"
                               if pinned else
                               f"add '{raw_target}' to platform_allow")
                            + " (issue #2209 class)"
                        )
    return problems


def find_problems(root: Path) -> list[str]:
    problems = _declared_core_overlay_problems(root)
    problems.extend(_platform_allow_overlay_problems(root))
    problems.extend(_stranded_boards_dir_problems(root))
    problems.extend(_same_pcb_sibling_undeclared_problems(root))
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
          "boards/ overlay wherever the example ships qualified overlays; "
          "every boards/ directory sits next to a CMakeLists.txt; and no "
          "scenario leaves a same-PCB sibling SKU's shipped overlay "
          "undeclared.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
