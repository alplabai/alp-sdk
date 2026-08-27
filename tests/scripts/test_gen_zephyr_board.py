# SPDX-License-Identifier: Apache-2.0
"""
Byte-for-byte regression gate for `scripts/gen_zephyr_board.py`
(`--emit zephyr-board`, issue #523).

Two things are pinned:

1. `emit_zephyr_board()` reproduces the committed `zephyr/boards/alp/*`
   board-tree files byte-for-byte, for every (SKU, core) this generator
   claims to fully or partially cover.  A change to the generator, the
   SoM presets, or the SoC JSON that drifts the committed board tree
   fails here -- the same "generated artefacts are byte-stable" contract
   `check_emit_snapshots.py` / `pr-generated-files.yml` hold for the
   other emitters.
2. The `scripts/alp_project.py --emit zephyr-board` CLI wiring actually
   writes those files to `--output`.

`e1m_v2n101_m33_sm` / `e1m_v2m101_m33_sm` are covered for only the three
family-agnostic files the generator produces for them today
(`board.yml`, `Kconfig.alp_<board>`, the twister `.yaml`) -- their
`.dts` / pinctrl `.dtsi` / `_defconfig` stay hand-authored (see the
module docstring in `gen_zephyr_board.py`) and are intentionally not
checked here.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from gen_zephyr_board import ZephyrBoardEmitError, _load_soc_spec, emit_zephyr_board  # noqa: E402

BOARDS_ROOT = REPO / "zephyr" / "boards" / "alp"

# Files that live in a generated board directory but are HAND-MAINTAINED, so
# `--emit zephyr-board` is not expected to write them.
#
# Everything else in that directory must round-trip byte-for-byte -- that is the
# whole point of this test, and the reason board files are generated from YAML
# rather than edited in place. So this set is an EXEMPTION LIST, not a
# convenience: a file added here stops being checked at all, and a generated
# file that quietly lands in it would drift forever with nothing red.
#
#   board.cmake  -- the runner/flash wiring, hand-written per board.
#   Kconfig      -- selects the custom E8 MPU region table. Zephyr's
#                   hwm_v2.cmake unconditionally osources a bare `Kconfig` in a
#                   board directory whether or not it exists, which is what lets
#                   a hand-maintained file survive regeneration here exactly as
#                   board.cmake does. Its own header says so; the GENERATED
#                   Kconfig files in the same directory are named
#                   `Kconfig.<board>` / `Kconfig.defconfig` and carry
#                   "DO NOT EDIT BY HAND", so the two are distinguishable by
#                   name rather than by convention.
HAND_MAINTAINED = frozenset({"board.cmake", "Kconfig"})
METADATA_ROOT = REPO / "metadata"

# The board directories whose committed tree is pinned byte-for-byte below.
# Single source of truth: the per-board test methods read this, and
# `test_every_committed_board_is_accounted_for` cross-checks it against the
# directories that actually exist under zephyr/boards/alp/.
PARITY_COVERED: dict[str, tuple[str, str]] = {
    "e1m_aen801_m55_hp": ("E1M-AEN801", "m55_hp"),
    "e1m_aen801_m55_he": ("E1M-AEN801", "m55_he"),
    "e1m_v2n101_m33_sm": ("E1M-V2N101", "m33_sm"),
    "e1m_v2m101_m33_sm": ("E1M-V2M101", "m33_sm"),
}

# Board directories that are committed but CANNOT be emitted at all today, with
# the specific missing generator input that blocks each one.  Issue #1332: these
# two were absent from the parity list with nothing recording why, so a
# generator change updated the four boards above and left these behind with no
# test red -- exactly what happened to the MRAM partition table in #1289.
#
# This is NOT a permanent exemption.  `test_non_emittable_boards_still_blocked`
# asserts each entry still raises, so the moment the missing input lands the
# test goes RED and tells you to move the board into PARITY_COVERED.  Nothing
# here is skipped silently.
NOT_EMITTABLE: dict[str, str] = {
    "e1m_aen401_m55_hp":
        "metadata/socs/alif/ensemble/e4.json has no `zephyr_peripherals_dtsi`; "
        "alp-sdk ships no zephyr/dts/alif/ensemble_e4_peripherals.dtsi for the "
        "E4's own peripheral/NPU node set (the E8 overlay is different silicon)",
    "e1m_aen601_m55_hp":
        "metadata/socs/alif/ensemble/e6.json has no `zephyr_peripherals_dtsi`; "
        "alp-sdk ships no zephyr/dts/alif/ensemble_e6_peripherals.dtsi for the "
        "E6's own peripheral/NPU node set (the E8 overlay is different silicon)",
}


def _sku_and_core(board_dir: str) -> tuple[str, str]:
    """`e1m_aen401_m55_hp` -> `("E1M-AEN401", "m55_hp")`."""
    parts = board_dir.split("_")
    return f"E1M-{parts[1].upper()}", "_".join(parts[2:])


class TestGenZephyrBoardByteEquivalence(unittest.TestCase):
    """Regenerate each covered board and diff every produced file against
    the committed tree, byte-for-byte."""

    def _assert_matches_committed(self, sku: str, core_id: str, board_dir: str) -> None:
        files = emit_zephyr_board(sku, core_id, METADATA_ROOT)
        self.assertTrue(files, f"generator produced no files for {sku}/{core_id}")
        committed_dir = BOARDS_ROOT / board_dir
        for relpath, content in files.items():
            _, fname = relpath.split("/", 1)
            committed_path = committed_dir / fname
            self.assertTrue(
                committed_path.is_file(),
                f"generator produced {fname!r} but no committed file exists "
                f"at {committed_path}")
            committed = committed_path.read_text(encoding="utf-8")
            self.assertEqual(
                committed, content,
                f"generated {fname} for {sku}/{core_id} drifted from the "
                f"committed {committed_path} -- regenerate or fix the source")

    def _parity(self, board_dir: str) -> None:
        sku, core_id = PARITY_COVERED[board_dir]
        self._assert_matches_committed(sku, core_id, board_dir)

    def test_aen801_m55_hp_full_tree(self) -> None:
        self._parity("e1m_aen801_m55_hp")

    def test_aen801_m55_he_full_tree(self) -> None:
        self._parity("e1m_aen801_m55_he")

    def test_v2n101_m33_sm_family_agnostic_files(self) -> None:
        self._parity("e1m_v2n101_m33_sm")

    def test_v2m101_m33_sm_family_agnostic_files(self) -> None:
        self._parity("e1m_v2m101_m33_sm")

    def test_every_committed_board_is_accounted_for(self) -> None:
        """No committed board tree may sit outside BOTH lists (#1332).

        The original defect was silence: `e1m_aen401_m55_hp` /
        `e1m_aen601_m55_hp` were simply absent, so nothing recorded that they
        were uncovered or why.  A newly added board directory now has to be
        placed deliberately -- pinned in PARITY_COVERED, or listed in
        NOT_EMITTABLE with its blocking reason.
        """
        committed = {p.name for p in BOARDS_ROOT.iterdir() if p.is_dir()}
        accounted = set(PARITY_COVERED) | set(NOT_EMITTABLE)
        self.assertEqual(
            committed - accounted, set(),
            "committed board tree(s) covered by neither PARITY_COVERED nor "
            "NOT_EMITTABLE -- add them to one (see #1332)")
        self.assertEqual(
            accounted - committed, set(),
            "PARITY_COVERED / NOT_EMITTABLE name board directories that do "
            "not exist under zephyr/boards/alp/")
        self.assertEqual(
            set(PARITY_COVERED) & set(NOT_EMITTABLE), set(),
            "a board cannot be both parity-pinned and non-emittable")

    def test_non_emittable_boards_still_blocked(self) -> None:
        """Each NOT_EMITTABLE entry must STILL fail to emit.

        This is what keeps the exemption from becoming permanent: once the
        missing input lands, `emit_zephyr_board()` succeeds, this test goes red,
        and the board has to be moved into PARITY_COVERED -- where a drift like
        #1289's stale MRAM partition table would have been caught.
        """
        for board_dir, reason in NOT_EMITTABLE.items():
            with self.subTest(board=board_dir):
                sku, core_id = _sku_and_core(board_dir)
                with self.assertRaises(
                        ZephyrBoardEmitError,
                        msg=(f"{board_dir} now emits -- move it from "
                             f"NOT_EMITTABLE into PARITY_COVERED and pin its "
                             f"committed tree (recorded blocker was: {reason})")):
                    emit_zephyr_board(sku, core_id, METADATA_ROOT)

    def test_aen_hand_maintained_files_stay_hand_authored(self) -> None:
        """`board.cmake` and the bare `Kconfig` are explicitly out of scope
        (see module docstring); confirm the generator doesn't claim either,
        so a future contributor who forgets the exclusion notices
        immediately.  `Kconfig`'s absence is otherwise SILENT -- the build
        succeeds on Zephyr's generic FLASH_0/SRAM_0 MPU fallback."""
        files = emit_zephyr_board("E1M-AEN801", "m55_hp", METADATA_ROOT)
        claimed = {relpath.split("/", 1)[1] for relpath in files}
        self.assertEqual(
            claimed & HAND_MAINTAINED, set(),
            "board.cmake / Kconfig should stay hand-authored -- see "
            "gen_zephyr_board.py's NOT GENERATED docstring section")

    def test_v2n_dts_pinctrl_defconfig_stay_hand_authored(self) -> None:
        """The Renesas-side GD32 supervisor pin wiring isn't in metadata
        yet, so these three files must NOT be claimed as generated."""
        files = emit_zephyr_board("E1M-V2N101", "m33_sm", METADATA_ROOT)
        claimed = {relpath.split("/", 1)[1] for relpath in files}
        self.assertEqual(
            claimed,
            {
                "board.yml",
                "Kconfig.alp_e1m_v2n101_m33_sm",
                "alp_e1m_v2n101_m33_sm_r9a09g056n48gbg_cm33.yaml",
            },
        )


AEN801_PRESET = "e1m_modules/E1M-AEN801.yaml"
E8_SOC = "socs/alif/ensemble/e8.json"


class _MutatedMetadata:
    """Copy `metadata/` to a temp dir so a test can mutate one fact.

    These generators read maintainer-authored metadata, so the only way to
    exercise a refusal is to author a bad SoM preset / SoC JSON. Mutating
    a throwaway copy keeps the repo's own metadata (and therefore every
    other test in this file) untouched.
    """

    def __enter__(self) -> "_MutatedMetadata":
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "metadata"
        shutil.copytree(METADATA_ROOT, self.root)
        return self

    def __exit__(self, *exc: object) -> None:
        self._tmp.cleanup()

    def sub(self, relpath: str, old: str, new: str) -> None:
        path = self.root / relpath
        text = path.read_text(encoding="utf-8")
        assert old in text, f"{old!r} not found in {relpath}"
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def drop_lines(self, relpath: str, needle: str) -> None:
        path = self.root / relpath
        kept = [ln for ln in path.read_text(encoding="utf-8").split("\n")
                if needle not in ln]
        path.write_text("\n".join(kept), encoding="utf-8")

    def json_set(self, relpath: str, key: str, value: object) -> None:
        path = self.root / relpath
        spec = json.loads(path.read_text(encoding="utf-8"))
        spec[key] = value
        path.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")

    def json_del(self, relpath: str, key: str) -> None:
        path = self.root / relpath
        spec = json.loads(path.read_text(encoding="utf-8"))
        spec.pop(key, None)
        path.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")

    def stage_overlay(self, relpath: str) -> None:
        """Place a file at `self.root.parent / "zephyr" / "dts" / relpath`.

        `_aen_peripherals_dtsi()`'s vintage probe looks next to
        *metadata_root* (`self.root`), not at the real repo checkout
        (#1354) -- a test exercising that branch has to stage the overlay
        in THIS tmp tree, not rely on alp-sdk's own `zephyr/` directory.
        """
        path = self.root.parent / "zephyr" / "dts" / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* test double, not a real overlay */\n",
                         encoding="utf-8")


class TestAenHardwareFactsComeFromMetadata(unittest.TestCase):
    """Every SKU, part designator, pin name and base address in a generated
    AEN board tree must be transcribed from metadata, never inherited from
    a sibling SKU.

    The E8 facts used to be generator constants ("Alif Ensemble E8", the
    `<alif/ensemble_e8_peripherals.dtsi>` include, "P3_4/P3_5",
    `0x80000000`) applied to every `E1M-AEN*` SKU, so following the
    documented new-AEN-SKU procedure emitted an E3 board tree labelled E8,
    including the E8 peripherals overlay, at exit 0 with no warning.
    """

    def test_part_designator_is_read_from_the_soc_json(self) -> None:
        with _MutatedMetadata() as mm:
            mm.json_set(E8_SOC, "part", "E9")
            files = emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
            dts = next(v for k, v in files.items() if k.endswith(".dts"))
            kconfig = files["alp_e1m_aen801_m55_hp/Kconfig.defconfig"]
            pinctrl = files[
                "alp_e1m_aen801_m55_hp/alp_e1m_aen801_m55_hp-pinctrl.dtsi"]
        self.assertIn("(Alif Ensemble E9, AE822FA0E5597LS0)", dts)
        self.assertIn("upstream Alif E9 SoC", dts)
        self.assertIn("The Ensemble E9 RTSS-HP has CONFIG_NUM_IRQS=480", kconfig)
        self.assertIn("(Alif Ensemble E9)", pinctrl)
        # The SoC-JSON path in the generated-file banner and the peripherals
        # overlay name legitimately still say `e8` -- they are the file names,
        # not the part designator. Nothing else may.
        for emitted in (dts, kconfig, pinctrl):
            self.assertNotIn("Ensemble E8", emitted)
            self.assertNotIn("Alif E8", emitted)

    def test_peripherals_overlay_is_read_from_the_soc_json(self) -> None:
        with _MutatedMetadata() as mm:
            mm.json_set(E8_SOC, "zephyr_peripherals_dtsi",
                        "alif/ensemble_e9_peripherals.dtsi")
            files = emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
            dts = next(v for k, v in files.items() if k.endswith(".dts"))
        self.assertIn("#include <alif/ensemble_e9_peripherals.dtsi>", dts)

    def test_soc_without_a_peripherals_overlay_is_refused(self) -> None:
        """A non-E8 Ensemble part must not silently inherit the E8's
        overlay: the E8 declares `ethosu85`, an E3 has 2x U55 and no U85.

        `ref` is mutated off `alif:ensemble:e8` too so this exercises the
        genuine authoring-gap message -- with `ref` left at E8 this is the
        VINTAGE shape instead, covered by
        `test_e8_missing_the_key_names_the_alp_sdk_vintage_not_the_som`
        below (#1354)."""
        with _MutatedMetadata() as mm:
            mm.json_del(E8_SOC, "zephyr_peripherals_dtsi")
            mm.json_set(E8_SOC, "ref", "alif:ensemble:e9")
            # Stage the E8 overlay too: in every real checkout it IS present
            # beside metadata/, so this must be refused on `ref != e8` alone,
            # not merely because no overlay happens to exist in this tmp
            # tree -- without this, deleting the `ref` check leaves the
            # suite green for the wrong reason (#1354 review round 2).
            mm.stage_overlay("alif/ensemble_e8_peripherals.dtsi")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        self.assertIn("zephyr_peripherals_dtsi", str(ctx.exception))
        self.assertIn("alif:ensemble:e9", str(ctx.exception))

    def test_e8_missing_the_key_names_the_alp_sdk_vintage_not_the_som(self) -> None:
        """#1352 added `zephyr_peripherals_dtsi` after the E8 overlay file
        (`zephyr/dts/alif/ensemble_e8_peripherals.dtsi`) already shipped, so
        every real checkout that has the field also has the file -- the old
        message ("Add the overlay ... before generating this board") told an
        E8 user on an old-but-real checkout to hand-author a 64+ KiB file
        that was already sitting in their own tree (#1354).

        The overlay is staged in THIS tmp tree (`mm.stage_overlay`), not
        read off alp-sdk's own `zephyr/` directory -- the probe judges the
        *metadata_root*'s tree, so a bare `json_del` here (with no overlay
        anywhere under `mm.root.parent`) must NOT be enough to trip the
        vintage branch; see
        `test_e8_missing_the_key_and_no_overlay_gets_the_authoring_message`
        below for that half."""
        with _MutatedMetadata() as mm:
            mm.json_del(E8_SOC, "zephyr_peripherals_dtsi")
            mm.stage_overlay("alif/ensemble_e8_peripherals.dtsi")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn(
            "this alp-sdk predates the per-SoC peripherals-overlay "
            "declaration", message)
        self.assertIn("alp-sdk#1352", message)
        self.assertIn("upgrade alp-sdk", message)
        self.assertIn("v0.16.0-rc1", message)
        self.assertNotIn("Add the overlay under zephyr/dts/alif/", message)

    def test_e8_missing_the_key_and_no_overlay_gets_the_authoring_message(
            self) -> None:
        """Same missing key as above, but with no overlay staged anywhere
        under `mm.root.parent` -- the `.is_file()` half of the vintage
        guard must still gate on it, not fire on `ref` alone.  Mutating
        that half away (`if soc_spec.get("ref") == "alif:ensemble:e8":`)
        left this branch's test suite fully green before this test existed
        (#1354 review)."""
        with _MutatedMetadata() as mm:
            mm.json_del(E8_SOC, "zephyr_peripherals_dtsi")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("Add the overlay under zephyr/dts/alif/", message)
        self.assertNotIn("this alp-sdk predates", message)

    def test_console_pads_in_the_defconfig_come_from_the_pinmux(self) -> None:
        """The `_defconfig` console comment used to hardcode the AEN801
        pads `P3_4/P3_5` while the sibling .dts derived them."""
        with _MutatedMetadata() as mm:
            mm.sub("pinmux/aen.yaml",
                   'silicon_peripheral: "UART5_RX_A", silicon_pad: "P3_4"',
                   'silicon_peripheral: "UART5_RX_A", silicon_pad: "P7_0"')
            mm.sub("pinmux/aen.yaml",
                   'silicon_peripheral: "UART5_TX_A", silicon_pad: "P3_5"',
                   'silicon_peripheral: "UART5_TX_A", silicon_pad: "P7_1"')
            files = emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
            defconfig = files[
                "alp_e1m_aen801_m55_hp/"
                "alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp_defconfig"]
        self.assertIn('(E1M edge "UART0", P7_0/P7_1)', defconfig)
        self.assertNotIn("P3_4", defconfig)


class TestAenMemoryMapValidation(unittest.TestCase):
    """The disjoint-slot0 branch copies `base` / `size_kib` straight out of
    the SoM preset into a `partition@` node.  Every way that can go wrong
    must raise, not emit."""

    def test_missing_atoc_names_the_alp_sdk_vintage_not_the_som(self) -> None:
        """An alp-sdk checkout from before #1289 has an otherwise-complete
        `memory_map:` and no `atoc`.  The old message ("memory_map is
        missing an integer-`base` region named 'atoc'") read as a defect in
        the consumer's own SoM metadata, sending them to a `board.yaml` and
        a preset that are both fine."""
        with _MutatedMetadata() as mm:
            mm.drop_lines(AEN801_PRESET, "name: atoc")
            mm.sub(AEN801_PRESET,
                   "name: storage,   base: 0x80560000, size_kib: 96",
                   "name: storage,   base: 0x80560000, size_kib: 128")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("this alp-sdk predates the SE-owned ATOC reservation", message)
        self.assertIn("alp-sdk#1289", message)
        self.assertIn("upgrade alp-sdk", message)
        self.assertIn("v0.16.0", message)

    def test_missing_non_atoc_region_still_reads_as_an_authoring_gap(self) -> None:
        with _MutatedMetadata() as mm:
            mm.drop_lines(AEN801_PRESET, "name: reserved")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("'reserved'", message)
        self.assertNotIn("predates", message)

    def test_half_authored_slot0_map_raises_instead_of_overlaying_the_sibling(self) -> None:
        """Dropping only `hp_slot0` used to fall back to the stock
        symmetric layout, putting `slot0_partition@10000` exactly on top of
        the `he_slot0` window the same file still declares -- silently
        undoing #1069."""
        with _MutatedMetadata() as mm:
            mm.drop_lines(AEN801_PRESET, "name: hp_slot0")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("'hp_slot0'", message)
        self.assertIn("#1069", message)

    def test_partition_outside_the_flash_node_raises(self) -> None:
        with _MutatedMetadata() as mm:
            mm.sub(AEN801_PRESET,
                   "name: storage,   base: 0x80560000, size_kib: 96",
                   "name: storage,   base: 0x80560000, size_kib: 2048")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        self.assertIn("outside the 5632 KiB App MRAM window", str(ctx.exception))

    def test_overlapping_regions_raise(self) -> None:
        with _MutatedMetadata() as mm:
            mm.sub(AEN801_PRESET,
                   "name: hp_slot0,  base: 0x802b0000",
                   "name: hp_slot0,  base: 0x802a0000")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        self.assertIn("overlap", str(ctx.exception))

    def test_mcuboot_off_the_mram_base_raises(self) -> None:
        """`mcuboot`'s base anchors the soc-nv-flash child's offset-0
        origin, but the child's own address was a hardcoded 0x80000000:
        a shifted map emitted every partition 32 KiB below its declared
        physical address, and the `.dts` and `_defconfig` disagreed."""
        with _MutatedMetadata() as mm:
            for base in (0x80000000, 0x80010000, 0x802B0000, 0x80550000,
                         0x80560000, 0x80578000):
                mm.sub(AEN801_PRESET, f"base: 0x{base:08x}",
                       f"base: 0x{base + 0x8000:08x}")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("anchors 'mcuboot' at 0x80008000", message)
        self.assertIn("0x80000000", message)


class TestSiliconVariantResolution(unittest.TestCase):
    def test_declared_variant_naming_no_order_code_raises(self) -> None:
        """A declared `silicon_variant:` is authoritative; falling through
        to the `alp_module_skus` reverse lookup emitted a whole board tree
        named after a part number the preset does not declare."""
        with _MutatedMetadata() as mm:
            mm.sub(AEN801_PRESET, "silicon_variant: AE822FA0E5597LS0",
                   "silicon_variant: AE822FA0E5597LSO")
            with self.assertRaises(ZephyrBoardEmitError) as ctx:
                emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        message = str(ctx.exception)
        self.assertIn("AE822FA0E5597LSO", message)
        self.assertIn("AE822FA0E5597LS0", message)

    def test_tbd_variant_still_falls_back_to_the_reverse_lookup(self) -> None:
        with _MutatedMetadata() as mm:
            mm.sub(AEN801_PRESET, "silicon_variant: AE822FA0E5597LS0",
                   "silicon_variant: TBD")
            files = emit_zephyr_board("E1M-AEN801", "m55_hp", mm.root)
        self.assertIn("    - name: ae822fa0e5597ls0\n",
                      files["alp_e1m_aen801_m55_hp/board.yml"])


class TestLoadSocSpecFailureShape(unittest.TestCase):
    """`_load_soc_spec()` migrated onto resolve_soc_path() (issue #1004);
    pins the ZephyrBoardEmitError shape that migration promised to keep."""

    def test_rejects_malformed_silicon_ref(self) -> None:
        with self.assertRaises(ZephyrBoardEmitError) as ctx:
            _load_soc_spec({"sku": "E1M-TEST", "silicon": "acme:widget"}, METADATA_ROOT)
        self.assertEqual(
            str(ctx.exception), "silicon ref 'acme:widget' is not a triple-colon string")


class TestZephyrBoardCli(unittest.TestCase):
    """`scripts/alp_project.py --emit zephyr-board` writes the board
    directory the docs describe."""

    def test_cli_writes_full_aen_he_tree(self) -> None:
        import tempfile

        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = Path(tmp) / "alp_e1m_aen801_m55_he"
            result = subprocess.run(
                [sys.executable, str(REPO / "scripts" / "alp_project.py"),
                 "--input", str(board_yaml),
                 "--core", "m55_he",
                 "--emit", "zephyr-board",
                 "--output", str(out_dir)],
                cwd=REPO, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            committed_dir = BOARDS_ROOT / "e1m_aen801_m55_he"
            for committed_file in committed_dir.iterdir():
                if committed_file.name in HAND_MAINTAINED:
                    continue
                generated_file = out_dir / committed_file.name
                self.assertTrue(generated_file.is_file(),
                                 f"CLI didn't write {committed_file.name}")
                self.assertEqual(
                    generated_file.read_text(encoding="utf-8"),
                    committed_file.read_text(encoding="utf-8"))

    def test_cli_requires_core(self) -> None:
        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--emit", "zephyr-board",
             "--output", "/tmp/should-not-be-written"],
            cwd=REPO, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--core", result.stderr)

    def test_cli_requires_output(self) -> None:
        board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
        result = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--core", "m55_he",
             "--emit", "zephyr-board"],
            cwd=REPO, capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--output", result.stderr)


class TestAenLogModeDefault(unittest.TestCase):
    """Every AEN board defaults LOG_MODE to LOG_MODE_MINIMAL (issue #1373).

    Zephyr's inherited `default LOG_MODE_DEFERRED` hands every record --
    and, via CONFIG_LOG_PRINTK, the Alp SDK banner with it -- to the log
    processing thread, which runs BELOW main and is therefore starved
    outright by the non-yielding busy-loop main() the AEN bench procedure
    requires (an idling M55 makes the Secure Enclave gate the DAP and the
    SE-UART).  Measured on E1M-AEN801 silicon: zero UART bytes out of a
    running, fault-free board.

    This is a FAMILY invariant, and only two of the four AEN board trees
    are generated -- e1m_aen401_m55_hp / e1m_aen601_m55_hp are
    hand-authored, so the byte-equivalence class above cannot see them
    drift.  Hence a check over every AEN board directory on disk rather
    than over the generator's output alone.
    """

    def _aen_board_dirs(self) -> list[Path]:
        dirs = sorted(p for p in BOARDS_ROOT.glob("e1m_aen*") if p.is_dir())
        self.assertTrue(dirs, f"no AEN board trees under {BOARDS_ROOT}")
        return dirs

    def test_every_aen_board_defaults_log_mode_minimal(self) -> None:
        for board_dir in self._aen_board_dirs():
            kdc = board_dir / "Kconfig.defconfig"
            self.assertTrue(
                kdc.is_file(),
                f"{board_dir.name} has no Kconfig.defconfig to carry the "
                f"LOG_MODE default")
            text = kdc.read_text(encoding="utf-8")
            self.assertIn(
                "choice LOG_MODE\n\tdefault LOG_MODE_MINIMAL\nendchoice\n", text,
                f"{kdc} lost its LOG_MODE_MINIMAL board default -- a "
                f"busy-loop main() on this board will print nothing (#1373)")

    def test_no_aen_defconfig_assigns_the_choice_symbol_directly(self) -> None:
        """`CONFIG_LOG_MODE_MINIMAL=y` in a board `_defconfig` reaches the
        same end state when CONFIG_LOG=y, but it also assigns an INVISIBLE
        choice symbol on every CONFIG_LOG=n build -- and 47 fragments under
        examples/aen/ set CONFIG_LOG=n.  Zephyr's
        scripts/kconfig/kconfig.py::check_assigned_choice_values() then
        warns "The choice symbol LOG_MODE_MINIMAL ... was selected (set
        =y), but no symbol ended up as the choice selection" on each one.
        The Kconfig.defconfig `default` is inert there, so keep the
        `_defconfig` free of it.
        """
        for board_dir in self._aen_board_dirs():
            for defconfig in board_dir.glob("*_defconfig"):
                text = defconfig.read_text(encoding="utf-8")
                self.assertNotIn(
                    "CONFIG_LOG_MODE_MINIMAL=y", text,
                    f"{defconfig} assigns the LOG_MODE choice symbol "
                    f"directly; use the Kconfig.defconfig `choice LOG_MODE / "
                    f"default LOG_MODE_MINIMAL` form instead (#1373)")


if __name__ == "__main__":
    unittest.main()
