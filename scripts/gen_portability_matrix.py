#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Regenerate the intra-family portability matrix in docs/portability-matrix.md.

The matrix is the empirical proof of the SDK's load-bearing customer
promise ("change `som.sku:` in `board.yaml`, rebuild, ship -- within a
SoM family").  Until Phase E.3 it was maintained by hand and would
silently drift as new SKUs landed; this generator re-runs the documented
swap-test recipe (docs/portability-matrix.md § Method) for every
(SKU x example) cell and rewrites the generated block of the doc:

  1. Discover the SoM SKUs of each family from the single-source presets
     metadata/e1m_modules/E1M-*.yaml (SKU-prefix grouping -- the same
     inline pure derivation scripts/alp_project.py and
     scripts/check_example_portability.py use).
  2. For each pinned portable example, copy its board.yaml to a temp
     dir, set `som.sku:` to the target SKU and remap `cores.<key>:` to
     the target preset's `topology:` keys (exact-key match first, then
     the unique same-OS-class key -- `board:` entries are Zephyr-class,
     `machine:` entries are Yocto-class).
  3. Run `scripts/alp_project.py --input <tmp> --core <key> --emit
     zephyr-conf` for every app-carrying core.  The cell is PASS iff
     every emit exits 0.
  4. Project per-SKU note tags straight from the preset metadata
     (memory.dram_mbit, inference.npu_population, on_module.npu /
     pcie_mux, status.partial_hw_config) -- never hand-typed.

Only the block between the BEGIN/END markers in the doc is rewritten;
the hand-written prose around it (method, expected-diff analysis, gap
history) survives regeneration.

Scope: a PASS cell means the config GENERATES cleanly for that
(SKU x example) pair -- the same gate the hand-run Phase A sweep used.
Byte-identity analysis of the emitted alp.conf across SKUs is
analytical prose and intentionally stays hand-maintained in the doc.

Usage:

    python3 scripts/gen_portability_matrix.py            # regenerate in place
    python3 scripts/gen_portability_matrix.py --check     # fail if out of sync
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("gen_portability_matrix: PyYAML is required.  "
             "Install via `pip install pyyaml`.")

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

METADATA = REPO / "metadata"
MODULES = METADATA / "e1m_modules"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"
DOC = REPO / "docs" / "portability-matrix.md"

# The ADR 0018 library-compatibility table reuses the orchestrator's own
# emit-side resolution as its single source of truth: a (library x SKU)
# cell is COMPATIBLE iff the very `requires:` check the emitter runs
# (`alp_orchestrate.libraries._check_requires`) accepts the SoM, and the
# manifest has an `integration:` section for an OS the SoM runs.  We never
# re-derive the compat rules here -- a library the emitter would reject
# renders incompatible by construction.
from alp_orchestrate.libraries import (  # noqa: E402
    _check_requires,
    available_libraries,
    load_manifest,
)
from alp_orchestrate.loader import _load_json, _silicon_to_soc_path  # noqa: E402
from alp_orchestrate.models import (  # noqa: E402
    BoardProject,
    OrchestratorError,
    Slice,
)
from alp_orchestrate.topology import _default_os_from_core_type  # noqa: E402

PASS = "✅"   # white heavy check mark
FAIL = "❌"   # cross mark
NA = "—"     # em dash: library not wireable on any OS this SoM runs

BEGIN_MARK = "<!-- BEGIN GENERATED: gen_portability_matrix -->"
END_MARK = "<!-- END GENERATED: gen_portability_matrix -->"

LIB_BEGIN_MARK = "<!-- BEGIN GENERATED: gen_portability_matrix_libraries -->"
LIB_END_MARK = "<!-- END GENERATED: gen_portability_matrix_libraries -->"

# The two SoM families, each with its pinned set of portable examples.
#
# SKU membership is discovered from metadata/e1m_modules/E1M-*.yaml by
# SKU prefix (a new SKU preset automatically grows a new row); the
# example set is a curated constant -- like gen_soc_caps.py's CAPS list
# -- because "which examples anchor the swap test" is a doc/test-design
# choice, not a hardware fact with a metadata home.  Cross-family
# portability (E1M <-> E1M-X) is intentionally NOT tested: separate
# product lines per docs/adr/0011-intra-family-portability.md.
FAMILIES: list[dict] = [
    {
        "title": "E1M family (Cortex-M-class)",
        "sku_prefixes": ("E1M-AEN", "E1M-NX9"),
        "examples": (
            ("i2c-scanner", "examples/peripheral-io/i2c-scanner"),
            ("gpio-button-led", "examples/peripheral-io/gpio-button-led"),
            ("pwm-led-fade", "examples/peripheral-io/pwm-led-fade"),
        ),
    },
    {
        "title": "E1M-X family (Cortex-A55 + Cortex-M33)",
        "sku_prefixes": ("E1M-V2N", "E1M-V2M"),
        "examples": (
            ("adc-voltmeter", "examples/peripheral-io/adc-voltmeter"),
            ("pwm-led-fade", "examples/peripheral-io/pwm-led-fade"),
            ("v2n-pwm-fan-control", "examples/v2n/v2n-pwm-fan-control"),
        ),
    },
]


class CellError(Exception):
    """A (SKU x example) cell could not be generated; str() is the reason."""


def load_presets() -> dict[str, dict]:
    """Map SKU -> parsed SoM preset for every metadata/e1m_modules/E1M-*.yaml."""
    presets: dict[str, dict] = {}
    for path in sorted(MODULES.glob("E1M-*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        sku = doc.get("sku") or path.stem
        presets[sku] = doc
    if not presets:
        raise SystemExit("gen_portability_matrix: no SoM presets found under "
                         f"{MODULES}")
    return presets


def family_skus(presets: dict[str, dict], prefixes: tuple[str, ...]) -> list[str]:
    """Sorted SKUs whose name starts with one of the family prefixes."""
    return sorted(s for s in presets if s.startswith(prefixes))


def _topology_class(entry: dict) -> str | None:
    """Classify a `topology:` entry: Zephyr slices carry `board:`,
    Yocto/Linux slices carry `machine:` (see the SoM preset schema)."""
    if "board" in entry:
        return "zephyr"
    if "machine" in entry:
        return "yocto"
    return None


def remap_cores(cores: dict, src_topology: dict, dst_topology: dict) -> dict:
    """Re-key a board.yaml `cores:` block onto the target SKU's topology.

    This is step 2 of the doc's Method: exact key matches are kept
    (`m33_sm` exists on every E1M-X SKU); a missing key is mapped to the
    UNIQUE same-OS-class key of the target topology (`m55_hp` on AEN ->
    `m33` on NX9101 -- both Zephyr-class).  Anything ambiguous fails the
    cell rather than guessing.
    """
    out: dict = {}
    for key, val in cores.items():
        if key in dst_topology:
            out[key] = val
            continue
        src_entry = src_topology.get(key)
        if not isinstance(src_entry, dict):
            raise CellError(f"core `{key}` is not in the source SKU topology")
        cls = _topology_class(src_entry)
        if cls is None:
            raise CellError(f"core `{key}` has no board:/machine: class")
        candidates = [k for k, v in dst_topology.items()
                      if isinstance(v, dict) and _topology_class(v) == cls
                      and k not in out and k not in cores]
        if len(candidates) != 1:
            raise CellError(
                f"core `{key}` ({cls}) has {len(candidates)} candidate keys "
                f"in the target topology -- cannot remap unambiguously")
        out[candidates[0]] = val
    return out


def run_cell(example_dir: Path, sku: str, presets: dict[str, dict],
             tmpdir: Path) -> None:
    """Run the swap test for one (SKU x example) cell; raise CellError on FAIL.

    Mirrors the doc's Method: rewrite som.sku + cores keys, then emit
    zephyr-conf for every app-carrying core via scripts/alp_project.py.
    """
    board_yaml = example_dir / "board.yaml"
    if not board_yaml.is_file():
        raise CellError(f"{board_yaml.relative_to(REPO)} does not exist")
    doc = yaml.safe_load(board_yaml.read_text(encoding="utf-8")) or {}

    src_sku = (doc.get("som") or {}).get("sku")
    src_preset = presets.get(src_sku)
    dst_preset = presets.get(sku)
    if src_preset is None:
        raise CellError(f"source SKU {src_sku!r} has no preset")
    if dst_preset is None:
        raise CellError(f"target SKU {sku!r} has no preset")

    doc.setdefault("som", {})["sku"] = sku
    doc["cores"] = remap_cores(doc.get("cores") or {},
                               src_preset.get("topology") or {},
                               dst_preset.get("topology") or {})

    emit_cores = [k for k, v in doc["cores"].items()
                  if isinstance(v, dict) and "app" in v]
    if not emit_cores:
        raise CellError("no app-carrying core to emit for")

    tmp_yaml = tmpdir / f"{sku}--{example_dir.name}.board.yaml"
    tmp_yaml.write_text(yaml.safe_dump(doc, sort_keys=True), encoding="utf-8")

    for core in emit_cores:
        proc = subprocess.run(
            [sys.executable, str(ALP_PROJECT),
             "--input", str(tmp_yaml),
             "--core", core,
             "--emit", "zephyr-conf"],
            capture_output=True, text=True, cwd=REPO,
        )
        if proc.returncode != 0:
            # Console gets the full diagnostic; the doc cell stays a
            # deterministic FAIL glyph (no temp paths in the output).
            print(f"gen_portability_matrix: {sku} x {example_dir.name} "
                  f"(core {core}) FAILED:\n{proc.stderr}", file=sys.stderr)
            raise CellError(f"`--emit zephyr-conf` failed for core `{core}`")


def notes_for(preset: dict) -> str:
    """Project the Notes cell from preset metadata only (never hand-typed).

    Tags, in a fixed order: DRAM density (memory.dram_mbit), on-SoC NPU
    population (inference.npu_population[].variant), on-module NPU +
    PCIe mux chips (on_module.npu / on_module.pcie_mux), and the
    status.partial_hw_config flag.
    """
    tags: list[str] = []

    dram = (preset.get("memory") or {}).get("dram_mbit")
    if isinstance(dram, int) and dram > 0:
        if dram >= 1024 and dram % 1024 == 0:
            tags.append(f"{dram // 1024} Gbit DRAM")
        else:
            tags.append(f"{dram} Mbit DRAM")

    population = (preset.get("inference") or {}).get("npu_population") or []
    variants = sorted({p.get("variant") for p in population
                       if isinstance(p, dict) and p.get("variant")})
    if variants:
        tags.append("Ethos-U " + "+".join(v.upper() for v in variants))

    on_module = preset.get("on_module") or {}
    if on_module.get("npu"):
        tags.append(f"NPU `{on_module['npu']}`")
    if on_module.get("pcie_mux"):
        tags.append(f"PCIe mux `{on_module['pcie_mux']}`")

    if (preset.get("status") or {}).get("partial_hw_config"):
        tags.append("`partial_hw_config: true`")

    return " · ".join(tags)


def sweep(presets: dict[str, dict],
          tmpdir: Path) -> list[tuple[str, list[str], dict[tuple[str, str], bool]]]:
    """Run every family's full (SKU x example) sweep.

    Returns [(title, skus, {(sku, example): passed})] in FAMILIES order.
    """
    results = []
    for fam in FAMILIES:
        skus = family_skus(presets, fam["sku_prefixes"])
        cells: dict[tuple[str, str], bool] = {}
        for sku in skus:
            for name, rel in fam["examples"]:
                try:
                    run_cell(REPO / rel, sku, presets, tmpdir)
                    cells[(sku, name)] = True
                except CellError as err:
                    print(f"gen_portability_matrix: {sku} x {name}: {err}",
                          file=sys.stderr)
                    cells[(sku, name)] = False
        results.append((fam["title"], skus, cells))
    return results


def render_block(results: list[tuple[str, list[str], dict]],
                 presets: dict[str, dict]) -> str:
    """Render the generated doc block (markers included), byte-stable."""
    lines: list[str] = [
        BEGIN_MARK,
        "<!-- AUTO-GENERATED by scripts/gen_portability_matrix.py "
        "— DO NOT EDIT between the markers. -->",
        "<!--",
        "     Regenerate after editing metadata/e1m_modules/E1M-*.yaml,",
        "     the pinned examples' board.yaml, or the emit pipeline",
        "     (scripts/alp_project.py + scripts/alp_orchestrate/):",
        "         python3 scripts/gen_portability_matrix.py",
        "     A CI gate (.github/workflows/pr-generated-files.yml) fails the",
        "     PR if this block drifts from what the swap-test produces.",
        "-->",
    ]

    for title, skus, cells in results:
        fam = next(f for f in FAMILIES if f["title"] == title)
        example_names = [name for name, _rel in fam["examples"]]

        lines.append("")
        lines.append(f"## {title}")
        lines.append("")

        header = ["SKU \\ Example", "Silicon", *example_names,
                  "Notes (from metadata)"]
        sep = ["---", "---", *([":---:"] * len(example_names)), "---"]
        lines.append("| " + " | ".join(header) + " |")
        lines.append("| " + " | ".join(sep) + " |")
        for sku in skus:
            preset = presets[sku]
            silicon = preset.get("silicon", "TBD")
            row = [sku, f"`{silicon}`",
                   *[PASS if cells[(sku, n)] else FAIL for n in example_names],
                   notes_for(preset)]
            lines.append("| " + " | ".join(row) + " |")

        total = len(skus) * len(example_names)
        passed = sum(1 for ok in cells.values() if ok)
        lines.append("")
        if passed == total:
            lines.append(f"**{passed} / {total} cells generate cleanly.**")
        else:
            lines.append(f"**{passed} / {total} cells generate cleanly "
                         f"({total - passed} FAILING — see the {FAIL} cells; "
                         "run `python3 scripts/gen_portability_matrix.py` "
                         "locally for the per-cell diagnostics).**")

    lines.append("")
    lines.append(f"Legend: {PASS} `--emit zephyr-conf` succeeds for every "
                 f"app-carrying core · {FAIL} it does not.")
    lines.append(END_MARK)
    return "\n".join(lines)


def replace_block(doc_text: str, block: str,
                  begin_mark: str = BEGIN_MARK,
                  end_mark: str = END_MARK) -> str:
    """Splice `block` between the given markers, preserving all prose."""
    begin = doc_text.find(begin_mark)
    end = doc_text.find(end_mark)
    if begin < 0 or end < 0:
        raise SystemExit(
            f"gen_portability_matrix: {DOC.relative_to(REPO)} is missing the "
            f"generated-block markers ({begin_mark!r} / {end_mark!r})")
    if end < begin:
        raise SystemExit(
            f"gen_portability_matrix: markers out of order in "
            f"{DOC.relative_to(REPO)}")
    return doc_text[:begin] + block + doc_text[end + len(end_mark):]


# ---------------------------------------------------------------------
# Library x SKU compatibility (ADR 0018)
# ---------------------------------------------------------------------


def _project_for_sku(sku: str, preset: dict) -> BoardProject:
    """Build the minimal BoardProject the library resolver needs for a SKU.

    Only the fields `_check_requires` reads are populated: `soc_spec`
    (RAM/flash/core-class facts), `som_preset` (capabilities), and one
    `Slice` per SoM `topology:` core carrying that core's effective OS
    (explicit `topology.<core>.os`, else the loader's core-type default).
    This is the SoM at full topology -- its maximum capability -- which is
    exactly the axis the SKU-row matrix already represents.
    """
    silicon = preset.get("silicon") or ""
    soc_spec: dict = {}
    if silicon:
        soc_path = _silicon_to_soc_path(silicon, METADATA)
        if soc_path.is_file():
            soc_spec = _load_json(soc_path)

    core_type_by_id = {
        str(c.get("id")): str(c.get("type") or "")
        for c in (soc_spec.get("cores") or []) if c.get("id")
    }
    cores: dict[str, Slice] = {}
    for core_id, entry in (preset.get("topology") or {}).items():
        entry = entry if isinstance(entry, dict) else {}
        os_ = entry.get("os") or _default_os_from_core_type(
            core_type_by_id.get(core_id, ""))
        cores[core_id] = Slice(core_id=core_id, os=os_)

    return BoardProject(
        sku=sku, hw_rev=None, board_name=None, board_hw_rev=None,
        cores=cores, ipc=[], soc_spec=soc_spec, som_preset=preset,
        board_preset=None,
    )


def _requirement_reason(message: str) -> str:
    """Compress a `_check_requires` error into a short, path-free cell reason.

    The authoritative messages all read
    ``library `x` requires <REQ>, but <target> ...`` /
    ``... requires <REQ>, which <target> ...``; the cell keeps only <REQ>
    (e.g. ``core_class `m```, ``min_ram_kib 64``) -- the SoM name is
    already the column header, so it is dropped to stay compact and
    deterministic.  This is a pure reformat of the resolver's own message,
    not a re-derivation of the decision.
    """
    marker = " requires "
    idx = message.find(marker)
    if idx < 0:
        return message.strip()
    rest = message[idx + len(marker):]
    for sep in (", but ", ", which "):
        cut = rest.find(sep)
        if cut >= 0:
            rest = rest[:cut]
            break
    return rest.strip()


def library_cell(name: str, manifest: dict, project: BoardProject) -> str:
    """Classify one (library x SKU) cell, reusing the emitter's resolution.

    Returns the rendered cell:
      * PASS               -- `requires:` satisfied AND wireable on this SoM
      * ``FAIL <reason>``  -- the emitter's `_check_requires` rejects the SoM
      * NA                 -- `requires:` OK but the manifest has no
                              `integration:` section for any OS this SoM runs
    """
    try:
        _check_requires(name, manifest, project, METADATA)
    except OrchestratorError as err:
        return f"{FAIL} {_requirement_reason(str(err))}"

    integration = manifest.get("integration") or {}
    oses = {s.os for s in project.cores.values() if s.os and s.os != "off"}
    if oses and not (set(integration) & oses):
        return NA
    return PASS


def library_sweep(
    presets: dict[str, dict],
) -> tuple[list[tuple[str, dict]], list[tuple[str, list[str], dict[tuple[str, str], str]]]]:
    """Resolve every (library x SKU) cell for every family.

    Returns ``(libraries, per_family)`` where ``libraries`` is
    ``[(name, manifest), ...]`` (sorted, auto-discovered) and
    ``per_family`` is ``[(title, skus, {(library, sku): rendered_cell})]``
    in FAMILIES order.
    """
    libraries = [(name, load_manifest(name, METADATA))
                 for name in available_libraries(METADATA)]

    projects = {sku: _project_for_sku(sku, preset)
                for sku, preset in presets.items()}

    per_family = []
    for fam in FAMILIES:
        skus = family_skus(presets, fam["sku_prefixes"])
        cells: dict[tuple[str, str], str] = {}
        for name, manifest in libraries:
            for sku in skus:
                cells[(name, sku)] = library_cell(name, manifest, projects[sku])
        per_family.append((fam["title"], skus, cells))
    return libraries, per_family


def render_library_block(
    libraries: list[tuple[str, dict]],
    per_family: list[tuple[str, list[str], dict[tuple[str, str], str]]],
) -> str:
    """Render the ADR 0018 library-compatibility block (markers included)."""
    lines: list[str] = [
        LIB_BEGIN_MARK,
        "<!-- AUTO-GENERATED by scripts/gen_portability_matrix.py "
        "— DO NOT EDIT between the markers. -->",
        "<!--",
        "     Rows are the ADR 0018 library manifests auto-discovered from",
        "     metadata/libraries/*.yaml; columns are each family's SoM SKUs.",
        "     Every cell reuses the orchestrator's own emit-side resolution",
        "     (alp_orchestrate/libraries.py) -- a library the emitter would",
        "     reject renders incompatible here.  Regenerate after editing a",
        "     manifest, a SoM preset, or the resolver:",
        "         python3 scripts/gen_portability_matrix.py",
        "-->",
    ]

    if not libraries:
        lines.append("")
        lines.append("_No library manifests under metadata/libraries/._")
        lines.append(LIB_END_MARK)
        return "\n".join(lines)

    for title, skus, cells in per_family:
        lines.append("")
        lines.append(f"### {title}")
        lines.append("")

        header = ["Library", "Tier", "Version", "License", *skus]
        sep = ["---", ":---:", "---", "---", *([":---:"] * len(skus))]
        lines.append("| " + " | ".join(header) + " |")
        lines.append("| " + " | ".join(sep) + " |")

        compat = incompat = na = 0
        for name, manifest in libraries:
            row = [
                f"`{name}`",
                str(manifest.get("tier", "?")),
                f"`{manifest.get('version', '?')}`",
                str(manifest.get("license", "?")),
            ]
            for sku in skus:
                cell = cells[(name, sku)]
                row.append(cell)
                if cell == PASS:
                    compat += 1
                elif cell == NA:
                    na += 1
                else:
                    incompat += 1
            lines.append("| " + " | ".join(row) + " |")

        total = len(libraries) * len(skus)
        lines.append("")
        if incompat == 0 and na == 0:
            lines.append(f"**{compat} / {total} (library × SKU) cells compatible.**")
        else:
            lines.append(
                f"**{compat} / {total} (library × SKU) cells compatible "
                f"({incompat} incompatible, {na} n/a).**")

    lines.append("")
    lines.append(
        f"Legend: {PASS} `requires:` satisfied and wireable on the SoM · "
        f"{FAIL} incompatible (the named `requires:` constraint fails) · "
        f"{NA} not applicable (no `integration:` for any OS this SoM runs).")
    lines.append(LIB_END_MARK)
    return "\n".join(lines)


def generate() -> str:
    """Produce the full regenerated doc text (both generated blocks)."""
    presets = load_presets()
    with tempfile.TemporaryDirectory(prefix="alp-portability-") as tmp:
        results = sweep(presets, Path(tmp))
    block = render_block(results, presets)

    libraries, per_family = library_sweep(presets)
    lib_block = render_library_block(libraries, per_family)

    text = DOC.read_text(encoding="utf-8")
    text = replace_block(text, block, BEGIN_MARK, END_MARK)
    text = replace_block(text, lib_block, LIB_BEGIN_MARK, LIB_END_MARK)
    return text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="fail (exit 1) if the matrix is out of sync with "
                         "the metadata + swap-test results")
    args = ap.parse_args()

    text = generate()

    if args.check:
        if DOC.read_text(encoding="utf-8") != text:
            print("gen_portability_matrix: docs/portability-matrix.md is out "
                  "of sync -- run `python3 scripts/gen_portability_matrix.py` "
                  "and commit the result.", file=sys.stderr)
            return 1
        print(f"OK   {DOC.relative_to(REPO)}  (in sync)")
        return 0

    DOC.write_text(text, encoding="utf-8")
    print(f"wrote {DOC.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
