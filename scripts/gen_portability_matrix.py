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
MODULES = REPO / "metadata" / "e1m_modules"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"
DOC = REPO / "docs" / "portability-matrix.md"

PASS = "✅"   # white heavy check mark
FAIL = "❌"   # cross mark

BEGIN_MARK = "<!-- BEGIN GENERATED: gen_portability_matrix -->"
END_MARK = "<!-- END GENERATED: gen_portability_matrix -->"

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


def replace_block(doc_text: str, block: str) -> str:
    """Splice `block` between the doc's markers, preserving all prose."""
    begin = doc_text.find(BEGIN_MARK)
    end = doc_text.find(END_MARK)
    if begin < 0 or end < 0:
        raise SystemExit(
            f"gen_portability_matrix: {DOC.relative_to(REPO)} is missing the "
            f"generated-block markers ({BEGIN_MARK!r} / {END_MARK!r})")
    if end < begin:
        raise SystemExit(
            f"gen_portability_matrix: markers out of order in "
            f"{DOC.relative_to(REPO)}")
    return doc_text[:begin] + block + doc_text[end + len(END_MARK):]


def generate() -> str:
    """Produce the full regenerated doc text."""
    presets = load_presets()
    with tempfile.TemporaryDirectory(prefix="alp-portability-") as tmp:
        results = sweep(presets, Path(tmp))
    block = render_block(results, presets)
    return replace_block(DOC.read_text(encoding="utf-8"), block)


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
