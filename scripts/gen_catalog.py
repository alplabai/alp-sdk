#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Generate metadata/catalog.json -- a single, machine-readable map of the
whole Alp SDK surface, for an AI agent / MCP server to read instead of
scanning the source tree.

This is a *projection*, not a new source of truth.  Every fact is derived
from an existing single source, so the catalog can never drift from the
SDK it describes (a CI regen-diff gate keeps it byte-in-sync):

  - soms          metadata/e1m_modules/E1M-*.yaml, each resolved to its
                  on-module SoC spec (metadata/socs/<v>/<f>/<part>.json)
                  via its `silicon:` ref -- the same SoM->SoC resolution +
                  peripheral-presence projection as gen_support_matrix.py,
                  plus a few named-instance keys (see
                  `_named_instance_presence`) for pad-routed silicon
                  instances the SoC-level class counts merge together.
  - examples      examples/<category>/<name>/board.yaml -- the example's
                  default SoM + board target, a one-line summary from its
                  README / main.c, and per-example filter facets
                  (schema_version 2, issue #1283): `cores[]` / `coreCount` /
                  `osSet` resolved through `core_os_topology()` -- the same
                  planner `alp_project.py --emit os-topology` calls, NOT the
                  raw board.yaml (47 of 100 examples never write `os:` on
                  ANY core at all, and of the other 53 only 2 name a
                  runtime `os:` directly -- the rest just turn a peer off --
                  so the YAML alone reports the wrong core count for almost
                  every example) -- plus a `declares` map of
                  cheap YAML-literal booleans (peripherals / chips / ipc /
                  models).  An example whose topology can't resolve (e.g. an
                  SoM hw_rev still `status: tbd`) omits the resolved facets
                  rather than guessing; `declares` stays present regardless.
  - emit_modes    the `--emit` artefact modes the orchestrator CLI exposes
                  (scripts/alp_orchestrate/cli.py) -- the ADR-0014 machine
                  contract.  The mode list is read from the CLI source so
                  it cannot drift; descriptions are maintained here.
  - portable_api  include/alp/*.h -- the public portable API: each header
                  and the `alp_*` functions it actually declares.
  - gates         scripts/check_*.py -- the validation gates, each with the
                  one-line purpose from its module docstring.

Scope is PRESENCE / STRUCTURE only.  Driver tier (Tier-1/2/3) and GA / stub
maturity are NOT structured metadata (tier lives in free-text driver `.c`
headers) and are deliberately out of scope -- the same deferral as
gen_support_matrix.py.  If a fact isn't in the metadata it is omitted, not
guessed.

The output is pretty-printed with sorted keys (byte-stable) and carries a
top-level `_generated` note + `schema_version`; it contains NO timestamp so
re-running on unchanged inputs reproduces it byte-for-byte.

Usage:

    python3 scripts/gen_catalog.py            # regenerate in place
    python3 scripts/gen_catalog.py --check     # fail (exit 1) if out of sync
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("gen_catalog: PyYAML is required.  Install via `pip install pyyaml`.")

REPO = Path(__file__).resolve().parent.parent
SCRIPTS = REPO / "scripts"

# Reuse gen_support_matrix's SoM->SoC resolution + peripheral-presence
# projection so the catalog's `peripherals` map is the SAME projection the
# support matrix renders (one source for the predicate set).
sys.path.insert(0, str(SCRIPTS))
from gen_support_matrix import (  # noqa: E402  (import after sys.path tweak)
    PERIPHERAL_CLASSES,
    load_modules,
    load_socs,
)

# Per-example facets (issue #1283) are resolved through the SAME planner the
# orchestrator CLI's `--emit os-topology` uses -- NOT read off the raw
# board.yaml.  47 of 100 examples never write `os:` on any core at all;
# reading the YAML literally reports the wrong core count / OS set for
# almost every example (see the module docstring below).
from alp_orchestrate import (  # noqa: E402
    OrchestratorError,
    SdkRevisionNotBuildable,
    core_os_topology,
    load_board_yaml,
)

MODULES = REPO / "metadata" / "e1m_modules"
EXAMPLES = REPO / "examples"
INCLUDE = REPO / "include" / "alp"
CLI = SCRIPTS / "alp_orchestrate" / "cli.py"
PINMUX = REPO / "metadata" / "pinmux"
OUT = REPO / "metadata" / "catalog.json"

SCHEMA_VERSION = 2

# Some SoC-level peripheral CLASSES (PERIPHERAL_CLASSES) merge multiple
# silicon instances into one count (e.g. n44.json's `sdio: 2` backs BOTH the
# SD1 card slot and the on-module WIFI_SDIO host controller -- there's no
# separate per-instance count to split).  Where the *routed pads* name the
# instances distinctly, project that pad-route evidence into its own
# catalog key instead of leaving the instance unrepresented.  Source:
# metadata/pinmux/<family>.yaml (generated from the real pinout TSVs, e.g.
# metadata/e1m_modules/v2n/renesas-peripheral-map.tsv:103-129) -- never a
# guessed split of the SoC count.
#
# SoM `family:` (metadata/e1m_modules/E1M-*.yaml) -> pin-mux family file.
# V2M reuses the V2N pinout in full (see the v2n-m1 comment in
# scripts/gen_pinmux_capability.py); NX9101 (nxp-imx9) has no pinmux table
# yet, so it is deliberately absent here, not guessed.
_SOM_FAMILY_TO_PINMUX_FAMILY: dict[str, str] = {
    "alif-ensemble":       "aen",
    "renesas-rzv2n":       "v2n",
    "renesas-rzv2n-deepx": "v2n",
}

# catalog key -> the `silicon_peripheral` name prefix that identifies the
# instance in the pin-mux table.
_NAMED_INSTANCE_PIN_PREFIXES: dict[str, str] = {
    "sd1":       "SD1",
    "wifi_sdio": "WIFI_SDIO",
}


def _named_instance_presence(som_family: str | None) -> dict[str, bool]:
    """Per-instance presence flags the SoC-level classes can't express,
    derived from the SoM family's routed pin-mux table (empty/False for a
    family with no pin-mux table -- absence of routing evidence, not a
    guess)."""
    pinmux_family = _SOM_FAMILY_TO_PINMUX_FAMILY.get(som_family or "")
    names: set[str] = set()
    if pinmux_family:
        path = PINMUX / f"{pinmux_family}.yaml"
        if path.is_file():
            doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            names = {p.get("silicon_peripheral") or "" for p in doc.get("pads", [])}
    return {
        key: any(name.startswith(prefix) for name in names)
        for key, prefix in _NAMED_INSTANCE_PIN_PREFIXES.items()
    }

_GENERATED = (
    "AUTO-GENERATED by scripts/gen_catalog.py -- DO NOT EDIT; regenerate with "
    "`python3 scripts/gen_catalog.py`.  Single-source, machine-readable map of "
    "the Alp SDK surface, projected from metadata/, examples/, include/alp/, "
    "and scripts/.  Presence/structure only (no driver tier or GA/stub status). "
    "A CI gate keeps it byte-in-sync with the sources."
)

# One-line description per `--emit` artefact mode.  The mode *list* is read
# from the CLI source (so it can't drift); these strings document them.
EMIT_MODE_DESCRIPTIONS: dict[str, str] = {
    "system-manifest":
        "Per-project system manifest (cores, slices, memory, helper "
        "firmware) the build + flash flow consumes.",
    "ipc-contract-h":
        "C header defining the inter-core IPC contract (shared-memory + "
        "mailbox channel layout).",
    "dts-reservations":
        "Devicetree /reserved-memory nodes for the cross-core shared-memory "
        "carve-outs.",
    "dts-partitions":
        "Devicetree flash/storage partition nodes for the project's memory "
        "layout.",
    "storage-mounts-c":
        "C source registering the project's storage mount points.",
    "tfm-sysbuild-conf":
        "TF-M sysbuild Kconfig fragment for the secure-processing-environment "
        "build.",
    "build-plan":
        "Per-core build plan (board target, app dir, toolchain) the "
        "orchestrator fans out.",
    "kconfig":
        "Board-scoped, user-settable Kconfig symbol menu for a --core "
        "slice (the vscode prj.conf LSP's live feed); needs a "
        "bootstrapped Zephyr workspace (ZEPHYR_BASE).",
}


def _slug(label: str) -> str:
    """Slugify a peripheral-class label into a stable snake_case key.

    e.g. "I2C" -> "i2c", "SDIO/eMMC" -> "sdio_emmc",
    "Quadrature Encoder" -> "quadrature_encoder".
    """
    return re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_")


def _topology(doc: dict) -> dict[str, dict]:
    """Project a SoM preset's `topology:` into a core_id -> facts map.

    The runtime class is structural: a core with a Yocto `machine:` runs
    Linux, a core with a Zephyr `board:` runs Zephyr (the same Cortex-A ->
    Yocto / Cortex-M -> Zephyr taxonomy the orchestrator enforces).  We
    report `os` derived from which target the preset declares, plus the raw
    `app` / `toolchain` / `board` / `machine` fields actually present.
    """
    topo: dict[str, dict] = {}
    for core_id, entry in (doc.get("topology") or {}).items():
        entry = entry or {}
        row: dict[str, str] = {}
        if "machine" in entry:
            row["os"] = "yocto"
            row["machine"] = entry["machine"]
        elif "board" in entry:
            row["os"] = "zephyr"
            row["board"] = entry["board"]
        if entry.get("app"):
            row["app"] = entry["app"]
        if entry.get("toolchain"):
            row["toolchain"] = entry["toolchain"]
        topo[core_id] = row
    return topo


def build_soms() -> list[dict]:
    """One entry per E1M SoM SKU, resolved to its SoC + peripheral map."""
    socs = load_socs()
    mods = load_modules()  # sorted [(sku, silicon_ref)]
    soms: list[dict] = []
    for sku, ref in mods:
        soc = socs.get(ref)
        if soc is None:
            raise SystemExit(
                f"gen_catalog: {sku} references silicon {ref!r} with no "
                f"matching metadata/socs/**.json (ref field)")
        doc = yaml.safe_load(
            (MODULES / f"{sku}.yaml").read_text(encoding="utf-8")) or {}
        peripherals = {
            _slug(label): bool(pred(soc))
            for label, pred in PERIPHERAL_CLASSES
        }
        peripherals.update(_named_instance_presence(doc.get("family")))
        soms.append({
            "sku":          sku,
            "silicon":      ref,
            "family":       doc.get("family"),
            "soc_part":     soc.get("part"),
            "topology":     _topology(doc),
            # #1243: renamed from `peripherals`, which read as "does
            # <alp/CLASS.h> work on this SKU" and is NOT what it means. It
            # is the literal silicon projection, vendor key spellings and
            # all -- `pwm: false` on V2N only means no SoC JSON uses the
            # key `pwm` (the silicon spells it `timer_32bit_gpt`), and
            # `dac: false` there is a true silicon fact that reads as the
            # opposite of reality, since <alp/dac.h> works via the GD32
            # bridge. The companion `portable_classes` map #1243 also asks
            # for is NOT emitted yet: it cannot be derived from
            # ALP_BACKEND_REGISTER today -- see the issue thread.
            "soc_peripherals":  peripherals,
            "capabilities": soc.get("capabilities") or {},
        })
    return soms


def _summary_from_readme(readme: Path) -> str | None:
    """First prose paragraph of an example README (heading stripped)."""
    if not readme.is_file():
        return None
    para: list[str] = []
    for raw in readme.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            if para:
                break
            continue
        if line.startswith("#"):  # skip ATX headings
            if para:
                break
            continue
        para.append(line)
    if not para:
        return None
    return re.sub(r"\s+", " ", " ".join(para)).strip()


def _summary_from_main_c(src_dir: Path) -> str | None:
    """First sentence of the lead block-comment in the example's main.c."""
    main_c = src_dir / "main.c"
    if not main_c.is_file():
        return None
    text = main_c.read_text(encoding="utf-8")
    m = re.search(r"/\*(.*?)\*/", text, re.DOTALL)
    if not m:
        return None
    body: list[str] = []
    for raw in m.group(1).splitlines():
        line = raw.strip().lstrip("*").strip()
        if line.startswith("Copyright") or line.startswith("SPDX"):
            continue
        if not line:
            if body:
                break
            continue
        body.append(line)
    if not body:
        return None
    joined = re.sub(r"\s+", " ", " ".join(body)).strip()
    return joined or None


def _declares(doc: dict) -> dict[str, bool]:
    """Cheap YAML-literal presence booleans (issue #1283) -- these ARE what
    the raw board.yaml says, unlike `cores`/`coreCount`/`osSet` below, so no
    topology resolution is needed to answer them honestly."""
    cores = doc.get("cores") or {}
    return {
        "peripherals": any((c or {}).get("peripherals") for c in cores.values()),
        "chips":       bool(doc.get("chips")),
        "ipc":         bool(doc.get("ipc")),
        "models":      bool(doc.get("models")),
    }


def _resolved_core_facets(board_yaml: Path) -> dict | None:
    """`cores[]` / `coreCount` / `osSet`, resolved through the SAME planner
    `alp_project.py --emit os-topology` calls (issue #1283) -- NOT the raw
    board.yaml.  A peer core left at its SoM topology default (47 of 100
    examples never write `os:` on any core at all) is enabled and carries
    a real `os` -- and an `app` when the resolved slice actually names one
    (including the SDK's own `alp-stock-shim` for an unused Zephyr peer)
    that the YAML alone never states.  `app` is OPTIONAL, not guaranteed: a
    Yocto slice can be a stock recipe (`image:` set, no `app:` -- schema-
    legal, see `alp_orchestrate.validate`) and then the core dict carries
    no `app` key at all.

    Returns None -- an honest absence, not a guess -- when the board can't
    be resolved at all.

    The EXPECTED case is silent: `SdkRevisionNotBuildable` means the SoM
    hw_rev exists but its `status:` refuses a build (`tbd` / `reserved` /
    no status key), which is exactly the exclusion
    `check_emit_snapshots.py:81-85` carves out for rpmsg-imx93.  Warning
    about it on every regen and every `--check` would be a permanent false
    alarm that trains the reader to ignore the channel.

    Any OTHER `OrchestratorError` is unexpected and warns on stderr rather
    than silently dropping the row's facets, so a regression is visible at
    regen time instead of getting committed as "in sync" by the next
    `--check` run.

    Discriminated by exception TYPE, not by string-matching the message.
    `SdkRevisionNotBuildable` exists as a subclass for precisely this
    reason -- see its docstring in `alp_orchestrate.models`, alongside
    `SdkRevisionUnsupported` and `SdkRevisionUnknown`."""
    try:
        project = load_board_yaml(board_yaml)
        topo = core_os_topology(project)
    except SdkRevisionNotBuildable:
        return None
    except OrchestratorError as exc:
        print(f"gen_catalog: {board_yaml.relative_to(REPO).as_posix()}: "
              f"os-topology did not resolve ({exc}) -- resolved facets "
              f"omitted for this example", file=sys.stderr)
        return None
    cores: list[dict] = []
    for row in topo["cores"]:
        if not row["enabled"]:
            continue
        core: dict = {"id": row["core_id"], "os": row["effective_os"]}
        slice_ = project.cores.get(row["core_id"])
        if slice_ is not None and slice_.app:
            core["app"] = slice_.app
        cores.append(core)
    return {
        "cores":     cores,
        "coreCount": len(cores),
        "osSet":     sorted({c["os"] for c in cores}),
    }


def build_examples() -> dict[str, list[dict]]:
    """Group examples/<category>/<name>/ (with a board.yaml) by category."""
    by_cat: dict[str, list[dict]] = {}
    for board_yaml in sorted(EXAMPLES.glob("*/*/board.yaml")):
        ex_dir = board_yaml.parent
        category = ex_dir.parent.name
        doc = yaml.safe_load(board_yaml.read_text(encoding="utf-8")) or {}
        summary = (_summary_from_readme(ex_dir / "README.md")
                   or _summary_from_main_c(ex_dir / "src"))
        entry: dict = {
            "path": ex_dir.relative_to(REPO).as_posix(),
            "name": ex_dir.name,
        }
        som = (doc.get("som") or {}).get("sku")
        if som:
            entry["som"] = som
        board = doc.get("preset") or doc.get("default_board")
        if board:
            entry["board"] = board
        if doc.get("supported_boards"):
            entry["supported_boards"] = list(doc["supported_boards"])
        if summary:
            entry["summary"] = summary
        facets = _resolved_core_facets(board_yaml)
        if facets is not None:
            entry.update(facets)
        entry["declares"] = _declares(doc)
        by_cat.setdefault(category, []).append(entry)
    for entries in by_cat.values():
        entries.sort(key=lambda e: e["path"])
    return by_cat


def build_emit_modes() -> list[dict]:
    """The orchestrator's `--emit` choices, read from the CLI source."""
    tree = ast.parse(CLI.read_text(encoding="utf-8"))
    choices: list[str] | None = None
    for node in ast.walk(tree):
        if (isinstance(node, ast.Call)
                and any(isinstance(a, ast.Constant) and a.value == "--emit"
                        for a in node.args)):
            for kw in node.keywords:
                if kw.arg == "choices" and isinstance(kw.value, (ast.List, ast.Tuple)):
                    choices = [e.value for e in kw.value.elts
                               if isinstance(e, ast.Constant)]
    if not choices:
        raise SystemExit(
            "gen_catalog: could not read --emit choices from "
            f"{CLI.relative_to(REPO).as_posix()}")
    modes: list[dict] = []
    for mode in sorted(choices):
        desc = EMIT_MODE_DESCRIPTIONS.get(mode)
        if desc is None:
            raise SystemExit(
                f"gen_catalog: --emit mode {mode!r} has no description in "
                "EMIT_MODE_DESCRIPTIONS (add one and regenerate).")
        modes.append({"mode": mode, "description": desc})
    return modes


# A public API symbol: an `alp_*` token immediately followed by `(`.  Matched
# against comment-stripped header text so @ref's and @code blocks don't count.
_FUNC_RE = re.compile(r"(?<![A-Za-z0-9_])(alp_[a-z][a-z0-9_]*)\s*\(")
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")


def _public_functions(header: Path) -> list[str]:
    """The `alp_*` function names a public header declares (sorted, unique)."""
    text = header.read_text(encoding="utf-8")
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub(" ", text)
    names = {m.group(1) for m in _FUNC_RE.finditer(text)
             if not m.group(1).endswith("_t")}  # _t tokens are types, not fns
    return sorted(names)


def build_portable_api() -> list[dict]:
    """One entry per include/alp/*.h, listing its public alp_* functions."""
    api: list[dict] = []
    for header in sorted(INCLUDE.glob("*.h")):
        api.append({
            "header":    header.relative_to(REPO).as_posix(),
            "functions": _public_functions(header),
        })
    return api


def _docstring_oneliner(script: Path) -> str | None:
    """First non-empty paragraph of a script's module docstring, collapsed."""
    try:
        doc = ast.get_docstring(ast.parse(script.read_text(encoding="utf-8")))
    except SyntaxError:
        return None
    if not doc:
        return None
    para: list[str] = []
    for line in doc.strip().splitlines():
        if line.strip():
            para.append(line.strip())
        elif para:
            break
    return re.sub(r"\s+", " ", " ".join(para)).strip() or None


def build_gates() -> list[dict]:
    """One entry per scripts/check_*.py validation gate."""
    gates: list[dict] = []
    for script in sorted(SCRIPTS.glob("check_*.py")):
        gates.append({
            "script":  script.relative_to(REPO).as_posix(),
            "purpose": _docstring_oneliner(script),
        })
    return gates


def build_catalog() -> dict:
    return {
        "_generated":     _GENERATED,
        "schema_version": SCHEMA_VERSION,
        "soms":           build_soms(),
        "examples":       build_examples(),
        "emit_modes":     build_emit_modes(),
        "portable_api":   build_portable_api(),
        "gates":          build_gates(),
    }


def render(catalog: dict) -> str:
    """Pretty-printed, sorted-key JSON -> byte-stable across runs."""
    return json.dumps(catalog, indent=2, sort_keys=True,
                      ensure_ascii=False) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if metadata/catalog.json is out of "
                         "sync with the sources")
    args = ap.parse_args()

    text = render(build_catalog())
    n_soms = text.count('"sku":')

    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.is_file() else ""
        if current != text:
            print("gen_catalog: metadata/catalog.json is out of sync -- run "
                  "`python3 scripts/gen_catalog.py` and commit the result.",
                  file=sys.stderr)
            return 1
        print(f"OK   {OUT.relative_to(REPO).as_posix()}  ({n_soms} SoMs, in sync)")
        return 0

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8", newline="")
    print(f"wrote {OUT.relative_to(REPO).as_posix()}  ({n_soms} SoMs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
