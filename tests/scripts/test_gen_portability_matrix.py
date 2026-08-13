"""Unit tests for scripts/gen_portability_matrix.py.

Covers determinism, marker/prose preservation, the --check gate, the
cores-remap rules, and known (SKU x example) cells anchored to the
committed metadata.  The full swap-test sweep runs real
`scripts/alp_project.py --emit zephyr-conf` subprocesses (~4 s total),
so it is executed once per module via a session-scoped fixture.
"""

import subprocess
import sys
from pathlib import Path

import pytest

import gen_portability_matrix as gpm  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_portability_matrix.py"
DOC = REPO / "docs" / "portability-matrix.md"


@pytest.fixture(scope="module")
def generated() -> str:
    """One full regenerated doc text, shared across the module's tests."""
    return gpm.generate()


def _cell(text: str, sku: str, column: str) -> str:
    """Return the raw cell for `sku` x `column` from whichever generated
    table contains a row for that SKU."""
    lines = text.splitlines()
    header = None
    for ln in lines:
        if ln.startswith("| SKU \\ Example "):
            header = [c.strip() for c in ln.strip("|").split("|")]
        if ln.startswith(f"| {sku} "):
            assert header is not None, f"row for {sku} appears before a header"
            cells = [c.strip() for c in ln.strip("|").split("|")]
            if column in header:
                return cells[header.index(column)]
    raise AssertionError(f"no cell for {sku} x {column}")


# ---------------------------------------------------------------------
# Determinism + committed-file sync (the actual CI gate contract)
# ---------------------------------------------------------------------


def test_generate_is_deterministic(generated):
    assert generated == gpm.generate()


def test_committed_doc_matches_generator(generated):
    assert DOC.read_text(encoding="utf-8") == generated


def test_check_mode_passes_on_committed_file():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


# ---------------------------------------------------------------------
# Marker handling: prose outside the block must survive regeneration
# ---------------------------------------------------------------------


def test_replace_block_preserves_surrounding_prose():
    doc = ("# Title\n\nhand prose BEFORE\n\n"
           f"{gpm.BEGIN_MARK}\nstale generated stuff\n{gpm.END_MARK}\n\n"
           "hand prose AFTER\n")
    block = f"{gpm.BEGIN_MARK}\nNEW\n{gpm.END_MARK}"
    out = gpm.replace_block(doc, block)
    assert "hand prose BEFORE" in out
    assert "hand prose AFTER" in out
    assert "NEW" in out
    assert "stale generated stuff" not in out
    # Re-splicing the same block is a fixpoint (idempotency of the marker
    # replacement itself, independent of the sweep).
    assert gpm.replace_block(out, block) == out


def test_replace_block_rejects_missing_markers():
    with pytest.raises(SystemExit):
        gpm.replace_block("no markers here\n", "block")


def test_committed_doc_keeps_hand_prose_outside_markers(generated):
    """The doc's hand-maintained sections live outside the generated block."""
    begin = generated.index(gpm.BEGIN_MARK)
    end = generated.index(gpm.END_MARK)
    prose = generated[:begin] + generated[end + len(gpm.END_MARK):]
    assert "## Method" in prose
    assert "## Hand-maintained analysis" in prose
    assert "## Status" in prose


# ---------------------------------------------------------------------
# Cores remap (Method step 2)
# ---------------------------------------------------------------------


def test_remap_cores_exact_key_kept():
    cores = {"m33_sm": {"app": "./src"}}
    topo = {"a55_cluster": {"machine": "x"}, "m33_sm": {"board": "y"}}
    assert gpm.remap_cores(cores, topo, topo) == cores


def test_remap_cores_maps_unique_same_class_key():
    # AEN m55_hp (Zephyr class) -> NX9101 m33 (its only Zephyr slice).
    src = {"a32_cluster": {"machine": "a"}, "m55_hp": {"board": "b"},
           "m55_he": {"board": "c"}}
    dst = {"a55_cluster": {"machine": "d"}, "m33": {"board": "e"}}
    out = gpm.remap_cores({"m55_hp": {"app": "./src"}}, src, dst)
    assert out == {"m33": {"app": "./src"}}


def test_remap_cores_ambiguous_target_fails():
    # Two Zephyr-class candidates in the target -> refuse to guess.
    src = {"m33": {"board": "a"}}
    dst = {"m55_hp": {"board": "b"}, "m55_he": {"board": "c"}}
    with pytest.raises(gpm.CellError):
        gpm.remap_cores({"m33": {"app": "./src"}}, src, dst)


# ---------------------------------------------------------------------
# Known cells + metadata-derived notes, anchored to committed metadata
# ---------------------------------------------------------------------


def test_known_pass_cells(generated):
    assert _cell(generated, "E1M-AEN801", "i2c-scanner") == gpm.PASS
    # E1M-X family: the AEN-authored pwm-led-fade lands on m33_sm.
    assert _cell(generated, "E1M-V2M102", "pwm-led-fade") == gpm.PASS


def test_nx9101_cells_fail_honestly_on_the_tbd_hw_rev(generated):
    """E1M-NX9101 x every pinned example is FAIL, not a remap bug.

    Was PASS (including the cross-core-class m55_hp -> m33 swap this
    once anchored) until #1025's hw_rev-buildable gate: NX9101's only
    hw_rev (imx93 r1) is `status: tbd`, so `--emit zephyr-conf` now
    refuses the SoM outright before it ever reaches core-remap or
    Kconfig generation. This is the gate doing its job, not a
    portability regression -- flip these back to PASS only alongside
    metadata/e1m_modules/imx93/hw-revisions.yaml:r1 gaining a buildable
    status; `test_remap_cores_maps_unique_same_class_key` above still
    covers the m55_hp -> m33 remap logic in isolation."""
    for example in ("i2c-scanner", "gpio-button-led", "pwm-led-fade"):
        assert _cell(generated, "E1M-NX9101", example) == gpm.FAIL, example


def test_notes_derive_from_metadata(generated):
    # AEN801 carries a U85 alongside its U55 pair (npu_population).
    assert "Ethos-U U55+U85" in _cell(generated, "E1M-AEN801",
                                      "Notes (from metadata)")
    # AEN701 is U55-only.
    assert "U85" not in _cell(generated, "E1M-AEN701",
                              "Notes (from metadata)")
    # V2M101 carries the DEEPX DX-M1 + PCIe mux (on_module).
    v2m = _cell(generated, "E1M-V2M101", "Notes (from metadata)")
    assert "deepx_dxm1" in v2m and "pi3dbs12212" in v2m
    # The 32 vs 64 Gbit DRAM variants render from memory.dram_mbit.
    assert "32 Gbit DRAM" in _cell(generated, "E1M-V2N101",
                                   "Notes (from metadata)")
    assert "64 Gbit DRAM" in _cell(generated, "E1M-V2N102",
                                   "Notes (from metadata)")


def test_every_family_sku_has_a_row(generated):
    presets = gpm.load_presets()
    for fam in gpm.FAMILIES:
        for sku in gpm.family_skus(presets, fam["sku_prefixes"]):
            assert f"| {sku} |" in generated


def test_notes_for_is_pure_metadata_projection():
    """notes_for never invents: empty preset -> empty notes."""
    assert gpm.notes_for({}) == ""
    assert gpm.notes_for({"memory": {"dram_mbit": "TBD"}}) == ""
    assert gpm.notes_for({"memory": {"dram_mbit": 65536}}) == "64 Gbit DRAM"


def test_v2n_v2m_memory_capacities_are_pinned():
    """#1230: `flash_mbit` has no generator/schema consumer beyond the
    required/type check in som-preset-v1.schema.json -- notes_for() above
    only ever renders dram_mbit, so nothing else in the gate suite catches
    a silent revert of flash_mbit (proven in round-5 review: reverting
    E1M-V2M101's flash_mbit 131072 -> 32768 alone left every generator/doc
    gate at rc=0; only alp.lock's raw-byte digest caught it, and a
    comment-only edit trips that identically). Pin the presets/fields whose
    memory values are settled so a revert of either field fails a real
    assertion here instead of relying solely on the digest.

    E1M-V2N102's and E1M-V2M102's `flash_mbit` are deliberately NOT pinned
    to a single value here: both trace only to non-independent in-tree
    sources -- `metadata/e1m_modules/E1M-V2N102.yaml` + `docs/soms/v2n.md`
    for the former, `metadata/e1m_modules/E1M-V2M102.yaml` +
    `docs/soms/v2n-m1.md` for the latter (each doc-side pair a same-day
    `git merge-base --is-ancestor`-confirmed descendant of its metadata
    commit, not independent corroboration) -- and neither has a maintainer
    ruling. `vendors/renesas-rzv2n/README.md` treats 128 Gbit and 64 Gbit
    as the two live candidates for both (128 Gbit is the value `d953e401`
    wrote there, itself a copy-paste across the V2N and V2N-M1 rows in one
    hunk, not a per-SKU spec transcription; 64 Gbit is what the metadata
    for both reads); #1230's ruling text
    ("its published capacities are 8 GB DRAM / 16 GB eMMC") is textually
    ambiguous about whether it covers E1M-V2M102 at all. Pinning either
    64 Gbit or 128 Gbit here would lock in an unverified, possibly-wrong
    number as if it were settled. Their `dram_mbit` IS pinned below --
    unlike `flash_mbit`, it isn't contested by any candidate figure."""
    presets = gpm.load_presets()
    expected = {
        "E1M-V2N101": {"dram_mbit": 32768, "flash_mbit": 32768},
        "E1M-V2M101": {"dram_mbit": 65536, "flash_mbit": 131072},
    }
    for sku, mem in expected.items():
        actual = presets[sku]["memory"]
        assert actual["dram_mbit"] == mem["dram_mbit"], sku
        assert actual["flash_mbit"] == mem["flash_mbit"], sku

    # dram_mbit is settled and uncontested for the two disputed-eMMC SKUs
    # too -- pin it on its own so a silent DRAM revert still fails here.
    dram_only = {"E1M-V2N102": 65536, "E1M-V2M102": 65536}
    for sku, dram_mbit in dram_only.items():
        assert presets[sku]["memory"]["dram_mbit"] == dram_mbit, sku

    # flash_mbit for these two is genuinely disputed between two candidate
    # figures (see the docstring above) -- pin it to that set instead of
    # dropping coverage entirely, so a regression to an unrelated number
    # (neither candidate) still fails here.
    disputed_flash_candidates = {
        "E1M-V2N102": {65536, 131072},
        "E1M-V2M102": {65536, 131072},
    }
    for sku, candidates in disputed_flash_candidates.items():
        assert presets[sku]["memory"]["flash_mbit"] in candidates, sku
