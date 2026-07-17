#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Cross-family portability lint for examples/*/{board.yaml,testcase.yaml}.

What this catches
-----------------

The Alp SDK's portability story is layered:

  Ring 1 -- cross-family examples.  Same main.c builds on every family
            it targets.  Declares no chip drivers in board.yaml, and
            its declared som.sku / supported_boards genuinely reach
            >= 2 SoM families; relies on <alp/peripheral.h> + the
            thin wrappers.

  Ring 2 -- chip-bound examples.  Uses <alp/chips/<chip>.h> but the
            chip is populated on multiple SoM families, so the
            example runs unchanged on any of them.

  Ring 3 -- SoM-bound examples.  Either uses a chip that only one
            family populates, or declares no chip but its som.sku /
            supported_boards resolve to exactly one family (e.g. a
            V2N-only board bring-up demo).  Customer can copy the
            example but it won't build cleanly on a different family.

The lint enforces two invariants:

  (a) HARD ERROR: an example's `chips:` list MUST NOT contain a
      chip whose `families:` field in metadata/chips/<chip>.yaml
      excludes the example's own som.sku family.  This catches the
      pathological case where someone wires `deepx_dxm1` into an
      AEN board.yaml; the build would fail downstream but the
      diagnostic would be cryptic.

  (b) INFO: classify each example into its ring and print the
      breakdown so a customer reading CI output (or the maintainer)
      can confirm the portability claim in examples/README.md
      matches reality.

      Rings 2 and 3 are an ACCEPTED, intentional category, not
      migration debt: a chip bring-up demo or a single-sensor /
      single-display tutorial is expected to `#include
      <alp/chips/<chip>.h>` directly and declare that chip in its
      `board.yaml` `chips:` list.  Only Ring 1 examples -- the
      general/portable ones -- are held to staying on
      `<alp/peripheral.h>` + `BOARD_*` aliases with no chip-driver
      include.  See docs/portability.md Sec 4.4 and
      examples/README.md for the customer-facing statement of this
      contract.

  (c) HARD ERROR: every board listed in `supported_boards:` must
      have a matching Twister variant in `testcase.yaml`, selected
      by the corresponding `ALP_BOARD_<SLUG>` compiler define.  The
      catalog claim and CI build matrix must not drift apart.

  (d) HARD ERROR: source files must use the pinout namespace that
      matches the target SoM form factor.  E1M examples may use
      <alp/e1m_pinout.h> / ALP_E1M_*; E1M-X examples may use
      <alp/e1m_x_pinout.h> / ALP_E1M_X_*.  Cross-EVK code should
      usually use <alp/board.h> BOARD_* aliases instead.

  (e) HARD ERROR (issue #520): a customer-facing example must not
      `#include <zephyr/drivers/...>` directly -- that's the vendor
      driver-class layer the portable `<alp/*.h>` surfaces (and
      `<alp/gui.h>`'s `alp_gui_lvgl_attach()` for LVGL) exist to hide.
      A handful of pre-existing examples genuinely have no portable
      surface to route through yet (raw AMP mailbox transport, MDIO
      PHY diagnostics, ...) -- those are named in
      `_ZEPHYR_DRIVER_INCLUDE_ALLOWLIST` below with the reason, not
      silently exempted.  Zephyr-specific board-bring-up / register
      bench tools that carry no `board.yaml` (e.g.
      `examples/aen/*-regcheck/`) are out of this check's scope
      entirely -- see the example-enumeration walk in `main()`.

  (f) HARD ERROR: every `examples/**/boards/<name>.overlay` and
      `tests/**/boards/<name>.overlay` must name a board that actually
      exists -- either a real in-tree target (a
      `zephyr/boards/alp/<dir>/board.yml`'s `board: name:` field) or a
      recognised upstream/host board (`native_sim`).  Zephyr resolves
      an overlay by the bare board name OR by board+qualifiers joined
      with underscores (e.g. `alp_e1m_aen801_m55_he_ae822fa0e5597ls0_
      rtss_he.overlay`), so a stem is valid if it equals a known name
      or starts with `<name>_`.  This check runs regardless of whether
      the example/test carries a `board.yaml` -- unlike (a)-(e) above,
      it is not scoped to `board.yaml`-bearing examples, so it also
      catches a dead overlay in a bare-Zephyr regcheck/test dir.

  (g) HARD ERROR: a customer-facing (`board.yaml`-bearing) example's
      `boards/<name>.overlay` must NOT name a bare board that has a
      fully-qualified sibling under `zephyr/boards/alp/<dir>/` (e.g.
      `alp_e1m_aen801_m55_he.overlay` when
      `alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.yaml` exists next
      to that board's `board.yml`).  Zephyr only auto-applies the
      overlay named after the FULLY-QUALIFIED board id on the customer
      build (`west build -b <bare>/<soc>/<core>`); a bare-name overlay
      builds "clean" and silently drops its devicetree edits.  Unlike
      (f), this check is scoped to `board.yaml`-bearing examples only
      -- an internal bench/regcheck dir with no `board.yaml` legitimately
      ships a bare-name overlay because `scripts/bench/aen/build.sh`
      force-applies it via `-DEXTRA_DTC_OVERLAY_FILE`.

Run from the alp-sdk repo root:

    python3 scripts/check_example_portability.py

Exits non-zero on a hard error.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from collections.abc import Iterator
from typing import Optional

try:
    import yaml
except ImportError:  # pragma: no cover -- CI always has pyyaml
    print("error: pyyaml not installed (pip install pyyaml)", file=sys.stderr)
    sys.exit(1)


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Map som.sku prefix -> family name as stored in chip metadata.
# E1M-AEN... -> aen
# E1M-V2N... -> v2n
# E1M-V2M... -> v2n-m1   (E1M-V2M is the V2N-M1 part numbering)
# E1M-NX9... -> imx93
_SKU_FAMILY_TABLE = (
    ("E1M-V2M", "v2n-m1"),
    ("E1M-V2N", "v2n"),
    ("E1M-AEN", "aen"),
    ("E1M-NX9", "imx93"),
)

_SKU_PINOUT_TABLE = (
    ("E1M-V2M", "e1m-x"),
    ("E1M-V2N", "e1m-x"),
    ("E1M-AEN", "e1m"),
    ("E1M-NX9", "e1m"),
)

# metadata/boards/<slug>.yaml's `hosts_som_families:` uses the
# vendor-style family names carried in the SoM preset's `family:`
# field (e.g. "renesas-rzv2n"); the chip-manifest `families:` lists
# (and _SKU_FAMILY_TABLE above) use the short slug ("v2n").  Mirror of
# the table in scripts/program_eeprom.py -- keep both in sync.
_VENDOR_FAMILY_TO_SLUG = {
    "alif-ensemble": "aen",
    "renesas-rzv2n": "v2n",
    "renesas-rzv2n-deepx": "v2n-m1",
    "nxp-imx9": "imx93",
}

_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
_SOURCE_SKIP_DIRS = {"build", ".git", ".west", "__pycache__"}
_E1M_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/e1m_pinout\.h[>"]')
_E1M_X_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/e1m_x_pinout\.h[>"]')
_E1M_TOKEN_RE = re.compile(r"\bALP_E1M_(?!X_)[A-Z0-9_]+\b")
_E1M_X_TOKEN_RE = re.compile(r"\bALP_E1M_X_[A-Z0-9_]+\b")
_CHIP_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/chips/([A-Za-z0-9_]+)\.h[>"]')
_ZEPHYR_DRIVER_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]zephyr/drivers/([A-Za-z0-9_./]+)\.h[>"]')

# Pre-existing examples that #include <zephyr/drivers/...> directly with no
# portable <alp/*.h> surface to route through today.  Keyed by the example's
# path relative to examples/ (matches how check_example()/main() identify
# examples).  Each entry names the include + WHY it's not a #520 migration
# target -- add a new entry only with a real "no portable surface exists"
# reason, not as a way to silence the gate.
_ZEPHYR_DRIVER_INCLUDE_ALLOWLIST: dict[str, str] = {
    "multicore/rpmsg-v2n": (
        "zephyr/drivers/mbox.h -- raw OpenAMP/MHU mailbox transport for "
        "AMP core-to-core messaging; no portable <alp/*.h> IPC surface "
        "exists yet."
    ),
    "peripheral-io/alp-console": (
        "zephyr/drivers/pwm.h (RGB status LED) and gpio.h/pinctrl.h "
        "(src/cc3501e_bridge.c, the on-module Wi-Fi/BLE bridge's own "
        "control-transport HAL) -- pre-existing gap predating #520 and "
        "out of its Display/LVGL scope; migrating the LED path to "
        "<alp/pwm.h> is tracked as separate follow-up work."
    ),
    "v2n/v2n-ethernet-dual": (
        "zephyr/drivers/mdio.h -- raw PHY register access for a link-"
        "diagnostics demo; no portable <alp/*.h> MDIO surface exists."
    ),
    "v2n/v2n-xspi-flash-readwrite": (
        "zephyr/drivers/flash.h -- no portable <alp/flash.h> surface "
        "exists yet."
    ),
}


def som_family_for_sku(sku: str) -> Optional[str]:
    """Return the SoM family slug for an E1M SKU, or None if unrecognised."""
    for prefix, family in _SKU_FAMILY_TABLE:
        if sku.startswith(prefix):
            return family
    return None


def pinout_namespace_for_sku(sku: str) -> Optional[str]:
    """Return the pinout namespace expected for the SoM form factor."""
    for prefix, namespace in _SKU_PINOUT_TABLE:
        if sku.startswith(prefix):
            return namespace
    return None


def _strip_block_comments(text: str) -> str:
    """Remove C block comments while preserving line numbers."""
    return re.sub(r"/\*.*?\*/",
                  lambda m: "\n" * m.group(0).count("\n"),
                  text,
                  flags=re.DOTALL)


def _strip_line_comments_and_literals(line: str) -> str:
    line = re.sub(r"//.*", "", line)
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
    return line


def source_files(example_dir: pathlib.Path) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for p in example_dir.rglob("*"):
        if not p.is_file() or p.suffix not in _SOURCE_SUFFIXES:
            continue
        rel_parts = p.relative_to(example_dir).parts
        if any(part in _SOURCE_SKIP_DIRS for part in rel_parts[:-1]):
            continue
        out.append(p)
    return sorted(out)


def check_pinout_namespace(example_dir: pathlib.Path,
                           namespace: Optional[str]) -> list[str]:
    """Return hard errors for E1M/E1M-X pinout namespace mismatches."""
    if namespace not in {"e1m", "e1m-x"}:
        return []

    errors: list[str] = []
    for src in source_files(example_dir):
        text = src.read_text(encoding="utf-8", errors="replace")
        rel = src.relative_to(example_dir).as_posix()
        without_blocks = _strip_block_comments(text)

        for line_no, line in enumerate(without_blocks.splitlines(), start=1):
            line_no_comment = re.sub(r"//.*", "", line)
            if namespace == "e1m-x" and _E1M_INCLUDE_RE.search(line_no_comment):
                errors.append(
                    f"{rel}:{line_no}: includes alp/e1m_pinout.h but "
                    "the example targets an E1M-X SoM; use "
                    "alp/e1m_x_pinout.h or alp/board.h"
                )
            elif namespace == "e1m" and _E1M_X_INCLUDE_RE.search(line_no_comment):
                errors.append(
                    f"{rel}:{line_no}: includes alp/e1m_x_pinout.h but "
                    "the example targets an E1M SoM; use "
                    "alp/e1m_pinout.h or alp/board.h"
                )

            code = _strip_line_comments_and_literals(line)
            if namespace == "e1m-x":
                match = _E1M_TOKEN_RE.search(code)
                if match:
                    errors.append(
                        f"{rel}:{line_no}: uses {match.group(0)} but "
                        "the example targets an E1M-X SoM; use ALP_E1M_X_* "
                        "or a BOARD_* alias"
                    )
            else:
                match = _E1M_X_TOKEN_RE.search(code)
                if match:
                    errors.append(
                        f"{rel}:{line_no}: uses {match.group(0)} but "
                        "the example targets an E1M SoM; use ALP_E1M_* "
                        "or a BOARD_* alias"
                    )
    return errors


def check_chip_includes_declared(example_dir: pathlib.Path,
                                 declared_chips: list) -> list[str]:
    """Return hard errors for `<alp/chips/*.h>` includes missing from
    board.yaml's `chips:` list (issue #514).

    The family-compatibility check (a) and the ring classification
    both walk only the declared `chips:` array -- an example that
    `#include`s a chip driver without declaring it slips past both:
    it can wire an incompatible chip with no diagnostic, and it can
    misreport its own portability ring as if it used no chip at all.
    """
    declared = set(declared_chips)
    errors: list[str] = []
    seen: set[str] = set()
    for src in source_files(example_dir):
        text = _strip_block_comments(
            src.read_text(encoding="utf-8", errors="replace"))
        rel = src.relative_to(example_dir).as_posix()
        for line_no, line in enumerate(text.splitlines(), start=1):
            match = _CHIP_INCLUDE_RE.match(re.sub(r"//.*", "", line))
            if not match:
                continue
            chip = match.group(1)
            if chip in declared or chip in seen:
                continue
            seen.add(chip)
            errors.append(
                f"{rel}:{line_no}: includes alp/chips/{chip}.h but "
                f"board.yaml has no matching chips: entry -- add '{chip}' "
                "so the family-compatibility check and portability ring "
                "can account for it"
            )
    return errors


def check_no_zephyr_driver_includes(example_dir: pathlib.Path,
                                    example_key: str) -> list[str]:
    """Return hard errors for `#include <zephyr/drivers/...>` in a
    customer-facing example (issue #520).

    `example_key` is the example's path relative to `examples/` (e.g.
    "display/lvgl-widgets-demo"), used to look it up in
    `_ZEPHYR_DRIVER_INCLUDE_ALLOWLIST`.  Allowlisted examples are
    skipped entirely -- their reason lives next to the allowlist entry,
    not scattered as inline suppressions.
    """
    if example_key in _ZEPHYR_DRIVER_INCLUDE_ALLOWLIST:
        return []

    errors: list[str] = []
    for src in source_files(example_dir):
        text = _strip_block_comments(
            src.read_text(encoding="utf-8", errors="replace"))
        rel = src.relative_to(example_dir).as_posix()
        for line_no, line in enumerate(text.splitlines(), start=1):
            line_no_comment = re.sub(r"//.*", "", line)
            match = _ZEPHYR_DRIVER_INCLUDE_RE.search(line_no_comment)
            if not match:
                continue
            errors.append(
                f"{rel}:{line_no}: includes <zephyr/drivers/{match.group(1)}.h> "
                "directly -- customer-facing examples must go through the "
                "portable <alp/*.h> surface (e.g. <alp/display.h> + "
                "alp_gui_lvgl_attach() for LVGL) instead. If no portable "
                f"surface exists yet, add '{example_key}' to "
                "_ZEPHYR_DRIVER_INCLUDE_ALLOWLIST in "
                f"{pathlib.Path(__file__).name} with why."
            )
    return errors


# Upstream/host Zephyr boards this SDK's examples/tests build against
# that ship in the Zephyr tree itself, not zephyr/boards/alp/ -- today
# that's just native_sim (native_sim/native/64), the CI/host simulation
# target every example builds against.
_UPSTREAM_BOARD_NAMES = {"native_sim"}


def load_real_board_names() -> set[str]:
    """Return every real Zephyr board target name shipped in-tree,
    read from each zephyr/boards/alp/<dir>/board.yml's `board: name:`
    field -- board EXISTENCE, not naming shape, is what makes a board
    name real; a stale overlay can name a board that was renamed (or
    never landed) before it shipped and still match the alp_e1m_*
    naming convention."""
    names: set[str] = set()
    boards_dir = ROOT / "zephyr" / "boards" / "alp"
    if not boards_dir.is_dir():
        return names
    for board_yml in sorted(boards_dir.glob("*/board.yml")):
        with board_yml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        name = (doc.get("board") or {}).get("name")
        if isinstance(name, str):
            names.add(name)
    return names


def overlay_board_name_error(overlay: pathlib.Path,
                             real_boards: set[str]) -> Optional[str]:
    """Return a hard-error string if `overlay`'s board-name stem names
    no real/known board, else None."""
    stem = overlay.stem
    known = real_boards | _UPSTREAM_BOARD_NAMES
    if stem in known:
        return None
    if any(stem.startswith(f"{name}_") for name in known):
        return None
    rel = overlay.relative_to(ROOT).as_posix()
    return (
        f"{rel}: overlay names unknown board '{stem}' -- not a real "
        f"zephyr/boards/alp/ board and not a recognised upstream/"
        f"native_sim board"
    )


def load_board_qualified_names() -> dict[str, set[str]]:
    """Map each board.yml's bare `name:` to the fully-qualified Zephyr
    board id(s) (`<board>_<soc>_<core>`) that ship next to it.

    Every `zephyr/boards/alp/<dir>/` carries exactly one twister board
    descriptor per SoC/core combo, named `<bare-name>_<soc>_<core>.yaml`
    alongside `board.yml`.  Scraping those sibling filenames (rather
    than hardcoding a naming scheme) is what lets this generalise past
    AEN801 to any current or future SoM whose board splits bare/
    qualified the same way.  A board with no such sibling (single SoC/
    core, no split) maps to an empty set -- its bare overlay name stays
    valid.
    """
    out: dict[str, set[str]] = {}
    boards_dir = ROOT / "zephyr" / "boards" / "alp"
    if not boards_dir.is_dir():
        return out
    for board_dir in sorted(p for p in boards_dir.iterdir() if p.is_dir()):
        board_yml = board_dir / "board.yml"
        if not board_yml.is_file():
            continue
        with board_yml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        bare_name = (doc.get("board") or {}).get("name")
        if not isinstance(bare_name, str):
            continue
        out[bare_name] = {p.stem for p in board_dir.glob(f"{bare_name}_*.yaml")}
    return out


def check_customer_overlay_qualified(
        example_dir: pathlib.Path,
        board_qualified_names: dict[str, set[str]]) -> list[str]:
    """HARD ERROR: a customer-facing (board.yaml-bearing) example's
    `boards/*.overlay` must not name a bare board that has a fully-
    qualified sibling -- Zephyr only auto-applies the overlay named
    after the FULLY-QUALIFIED board id on `west build -b
    <bare>/<soc>/<core>` (the customer invocation for a catalog
    example); a `boards/<bare-name>.overlay` silently drops with no
    build error.  4 customer AEN examples hit exactly this before being
    renamed to the qualified form.

    Internal bench/regcheck examples carry no board.yaml and are out of
    scope here -- scripts/bench/aen/build.sh force-applies the
    bare-name overlay itself via -DEXTRA_DTC_OVERLAY_FILE and is
    unaffected by Zephyr's board-target auto-apply rule.
    """
    boards_dir = example_dir / "boards"
    if not boards_dir.is_dir():
        return []
    errors: list[str] = []
    for overlay in sorted(boards_dir.glob("*.overlay")):
        qualified = board_qualified_names.get(overlay.stem)
        if not qualified:
            continue
        try:
            rel = overlay.relative_to(ROOT).as_posix()
        except ValueError:
            rel = overlay.as_posix()
        errors.append(
            f"{rel}: overlay names bare board '{overlay.stem}' but this "
            "example has a board.yaml (customer-facing) -- Zephyr only "
            "auto-applies the fully-qualified overlay on `west build -b "
            f"{overlay.stem}/<soc>/<core>`; rename to one of "
            f"{sorted(qualified)}"
        )
    return errors


def check_dead_board_overlays(real_boards: set[str]) -> list[str]:
    """Return hard errors for every examples/**/boards/*.overlay and
    tests/**/boards/*.overlay naming a board that doesn't exist.

    Runs independent of check_example()'s board.yaml-scoped walk --
    every overlay under a `boards/` dir counts, including the
    board.yaml-less regcheck/test scenarios that (a)-(e) above skip.
    """
    errors: list[str] = []
    for root_name in ("examples", "tests"):
        base = ROOT / root_name
        if not base.is_dir():
            continue
        for overlay in sorted(base.glob("**/boards/*.overlay")):
            rel_parts = overlay.relative_to(ROOT).parts
            if any(part in _SOURCE_SKIP_DIRS for part in rel_parts):
                continue
            error = overlay_board_name_error(overlay, real_boards)
            if error:
                errors.append(error)
    return errors


def load_chip_families() -> dict[str, list[str]]:
    """Map chip_id -> families list, scraped from metadata/chips/*.yaml."""
    out: dict[str, list[str]] = {}
    for chip_yaml in sorted((ROOT / "metadata" / "chips").glob("*.yaml")):
        with chip_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        chip_id = doc.get("chip_id")
        families = doc.get("families", [])
        if isinstance(chip_id, str) and isinstance(families, list):
            out[chip_id] = [str(f) for f in families]
    return out


def load_som_optional_chips() -> dict[str, set[str]]:
    """Map som.sku -> set of chip_ids whose `assembled` flag isn't a hard
    true.  Used to surface "chip is BOM-optional on this SKU" warnings."""
    out: dict[str, set[str]] = {}
    for som_yaml in sorted((ROOT / "metadata" / "e1m_modules").glob("E1M-*.yaml")):
        with som_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        sku = doc.get("sku")
        if not isinstance(sku, str):
            continue
        optional: set[str] = set()
        i2c_buses = ((doc.get("on_module") or {}).get("i2c_devices") or {})
        for bus_doc in i2c_buses.values():
            if not isinstance(bus_doc, dict):
                continue
            for dev in bus_doc.get("devices", []) or []:
                if not isinstance(dev, dict):
                    continue
                assembled = dev.get("assembled", True)
                chip = dev.get("chip")
                if not isinstance(chip, str):
                    continue
                # BOM-variant population: anything that's not a hard True
                # means the chip might not be present on every unit of
                # this SKU.  False = DNI; "optional" = per BOM variant.
                if assembled is not True:
                    optional.add(chip)
        out[sku] = optional
    return out


def load_board_host_families() -> dict[str, set[str]]:
    """Map board preset slug -> the set of chip-family slugs
    (aen / v2n / v2n-m1 / imx93) it hosts.

    Scraped from metadata/boards/<slug>.yaml's `hosts_som_families:`
    and translated through _VENDOR_FAMILY_TO_SLUG.  Used to decide
    whether a no-chip example's `supported_boards:` fan-out actually
    reaches more than one SoM family (issue #519) -- an entry like
    `e1m-x-evk` alone already spans two families (v2n, v2n-m1).
    """
    out: dict[str, set[str]] = {}
    for board_yaml in sorted((ROOT / "metadata" / "boards").glob("*.yaml")):
        with board_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        hosts = doc.get("hosts_som_families") or []
        if not isinstance(hosts, list):
            continue
        out[board_yaml.stem] = {
            _VENDOR_FAMILY_TO_SLUG.get(str(h), str(h)) for h in hosts
        }
    return out


def board_define_for_supported_board(board: str) -> str:
    """Return the compiler define selected by a supported_boards entry."""
    return f"ALP_BOARD_{board.upper().replace('-', '_')}"


def _iter_yaml_strings(value: object) -> Iterator[str]:
    """Yield scalar strings from parsed YAML data."""
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from _iter_yaml_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from _iter_yaml_strings(item)


def _testcase_strings(testcase_yaml: pathlib.Path) -> tuple[list[str], Optional[str]]:
    try:
        with testcase_yaml.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
    except yaml.YAMLError as exc:
        return [], f"testcase.yaml is not valid YAML: {exc}"
    return list(_iter_yaml_strings(doc)), None


def check_supported_board_testcases(
    example_dir: pathlib.Path,
    supported_boards: object,
) -> list[str]:
    """Verify supported_boards has explicit ALP_BOARD_* testcase variants."""
    if supported_boards in (None, []):
        return []
    if not isinstance(supported_boards, list):
        return ["supported_boards must be a list"]

    testcase_yaml = example_dir / "testcase.yaml"
    if not testcase_yaml.exists():
        return [
            "supported_boards is declared but testcase.yaml is missing -- "
            "add one Twister scenario per supported board"
        ]

    strings, parse_error = _testcase_strings(testcase_yaml)
    if parse_error:
        return [parse_error]

    testcase_text = "\n".join(strings)
    errors: list[str] = []
    for board in supported_boards:
        if not isinstance(board, str):
            errors.append(f"supported_boards entry {board!r} is not a string")
            continue
        define = board_define_for_supported_board(board)
        if define not in testcase_text:
            errors.append(
                f"supported_boards declares '{board}' but testcase.yaml has no "
                f"{define} variant"
            )
    return errors


def _no_chip_ring(family: Optional[str],
                  supported_boards: object,
                  board_host_families: dict[str, set[str]]) -> str:
    """Classify a chip-less example (issue #519).

    A `chips:`-less board.yaml carries no chip-population constraint,
    but that does NOT make it cross-family by default -- it's only
    ring1 if the example's own declarations (som.sku's family plus any
    supported_boards fan-out) actually reach >= 2 SoM families.  A
    single-SoM demo (e.g. a V2N-only eMMC/xSPI/PWM bring-up example)
    with no supported_boards stays SoM-bound -- ring3.
    """
    families: set[str] = set()
    if family:
        families.add(family)
    if isinstance(supported_boards, list):
        for board in supported_boards:
            if isinstance(board, str):
                families |= board_host_families.get(board, set())

    if len(families) >= 2:
        return "ring1-cross-family"
    if len(families) == 1:
        return "ring3-som-bound"
    # som.sku is missing/unrecognised and supported_boards resolved
    # nothing -- can't make a portability claim either way.
    return "ring-unknown"


def classify(chip_families: dict[str, list[str]],
             example_chips: list[str],
             family: Optional[str] = None,
             supported_boards: object = None,
             board_host_families: Optional[dict[str, set[str]]] = None) -> str:
    """Classify an example into ring1 / ring2 / ring3."""
    if not example_chips:
        return _no_chip_ring(family, supported_boards,
                             board_host_families or {})

    # Collect the intersection of families across every chip the
    # example references.  An example runs on a family iff every
    # chip it lists supports that family.
    family_sets = [set(chip_families.get(c, [])) for c in example_chips]
    if not all(family_sets):
        # At least one chip we couldn't resolve -- treat as unknown ring.
        return "ring-unknown"
    intersection = set.intersection(*family_sets) if family_sets else set()

    if len(intersection) >= 2:
        return "ring2-chip-bound"  # runs on >= 2 families
    return "ring3-som-bound"        # runs on exactly one family


def check_example(example_dir: pathlib.Path,
                  chip_families: dict[str, list[str]],
                  som_optional: dict[str, set[str]],
                  board_host_families: Optional[dict[str, set[str]]] = None,
                  board_qualified_names: Optional[dict[str, set[str]]] = None
                  ) -> tuple[str, list[str], list[str]]:
    """Return (classification, hard-error list, info-level note list)."""
    if board_host_families is None:
        board_host_families = load_board_host_families()
    if board_qualified_names is None:
        board_qualified_names = load_board_qualified_names()
    board_yaml = example_dir / "board.yaml"
    if not board_yaml.exists():
        return "no-board-yaml", [], []
    with board_yaml.open(encoding="utf-8") as f:
        doc = yaml.safe_load(f) or {}

    som_sku = (doc.get("som") or {}).get("sku", "")
    family  = som_family_for_sku(som_sku)
    pinout_namespace = pinout_namespace_for_sku(som_sku)
    chips   = doc.get("chips") or []

    errors: list[str] = []
    notes:  list[str] = []

    errors.extend(check_supported_board_testcases(example_dir,
                                                  doc.get("supported_boards")))

    if family is None and som_sku:
        errors.append(
            f"unknown som.sku prefix '{som_sku}' -- can't classify; "
            f"add the prefix to _SKU_FAMILY_TABLE in {pathlib.Path(__file__).name}"
        )

    if pinout_namespace is None and som_sku:
        errors.append(
            f"unknown som.sku prefix '{som_sku}' -- can't validate the "
            f"E1M/E1M-X pinout namespace; add the prefix to "
            f"_SKU_PINOUT_TABLE in {pathlib.Path(__file__).name}"
        )

    # SDK-level block helpers live under `blocks/<name>/`, not
    # `chips/<name>/`, so they don't have a metadata/chips/<name>.yaml.
    # Skip them in the families[] check -- block helpers are
    # SoM-family-agnostic by design (they're software abstractions
    # over GPIO / PDM, not third-party ICs).
    _BLOCK_SLUGS = {"button_led", "pdm_mic"}

    if family is not None:
        for chip in chips:
            if chip in _BLOCK_SLUGS:
                continue
            families = chip_families.get(chip)
            if families is None:
                errors.append(
                    f"chips: references '{chip}' but no metadata/chips/{chip}.yaml "
                    f"declares its families[] -- add the manifest"
                )
            elif family not in families:
                errors.append(
                    f"chips: references '{chip}' (families={families}) but the "
                    f"example targets som.sku '{som_sku}' (family '{family}') -- "
                    f"this example won't build on the configured SoM"
                )

        # BOM-variant note: the chip is wired into the example but the
        # SKU marks it as `assembled: optional` (or false).  The example
        # should handle the `_init() == ALP_ERR_NOT_READY` path
        # gracefully on those BOM variants.  Not a hard error.
        for chip in chips:
            if chip in som_optional.get(som_sku, set()):
                notes.append(
                    f"chip '{chip}' is BOM-optional on {som_sku} -- main.c "
                    f"should handle alp_*_init returning ALP_ERR_NOT_READY"
                )

    errors.extend(check_pinout_namespace(example_dir, pinout_namespace))
    errors.extend(check_chip_includes_declared(example_dir, chips))
    try:
        example_key = example_dir.relative_to(ROOT / "examples").as_posix()
    except ValueError:
        # example_dir isn't under the real examples/ tree (e.g. a unit
        # test fixture in a tmp dir) -- fall back to the leaf name so
        # the allowlist lookup still degrades sensibly instead of
        # raising.
        example_key = example_dir.name
    errors.extend(check_no_zephyr_driver_includes(example_dir, example_key))
    errors.extend(check_customer_overlay_qualified(example_dir,
                                                   board_qualified_names))

    ring = classify(chip_families, chips, family,
                    doc.get("supported_boards"), board_host_families)
    return ring, errors, notes


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress the per-example classification table",
    )
    args = parser.parse_args(argv)

    chip_families = load_chip_families()
    som_optional  = load_som_optional_chips()
    board_host_families = load_board_host_families()
    board_qualified_names = load_board_qualified_names()

    examples_dir = ROOT / "examples"
    # Walk one OR two levels deep: cross-family examples live directly
    # at examples/<name>/, SoM-specific examples live under
    # examples/<family>/<name>/ (e.g. examples/v2n/v2n-gd32-bridge-ping).
    examples: list = []
    for d in examples_dir.iterdir():
        if not d.is_dir():
            continue
        if (d / "board.yaml").exists():
            examples.append(d)
        else:
            for sub in d.iterdir():
                if sub.is_dir() and (sub / "board.yaml").exists():
                    examples.append(sub)
    examples.sort()

    if not examples:
        print(f"error: no examples found under {examples_dir}", file=sys.stderr)
        return 1

    hard_errors_total = 0
    classification: dict[str, list[str]] = {}

    for ex in examples:
        ring, errors, notes = check_example(ex, chip_families, som_optional,
                                            board_host_families,
                                            board_qualified_names)
        classification.setdefault(ring, []).append(ex.name)
        for e in errors:
            print(f"FAIL  {ex.name}: {e}", file=sys.stderr)
            hard_errors_total += 1
        for n in notes:
            print(f"NOTE  {ex.name}: {n}")

    real_boards = load_real_board_names()
    for e in check_dead_board_overlays(real_boards):
        print(f"FAIL  {e}", file=sys.stderr)
        hard_errors_total += 1

    if not args.quiet:
        print()
        print("Portability classification:")
        for ring in sorted(classification):
            print(f"  {ring} ({len(classification[ring])}):")
            for name in sorted(classification[ring]):
                print(f"    - {name}")
        print()

    if hard_errors_total:
        print(f"{hard_errors_total} hard error(s) -- failing.", file=sys.stderr)
        return 1

    print(f"OK: {len(examples)} example(s) checked, 0 portability errors.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
