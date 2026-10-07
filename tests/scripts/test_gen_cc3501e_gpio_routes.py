# SPDX-License-Identifier: Apache-2.0
"""Tests for `scripts/gen_cc3501e_gpio_routes.py` (issue #1859).

Covers:
- `_proxy_rows()` drops a composed route whose dispatch_pin is
  bridge-reserved (the generation-time guard replacing the old
  hand-picked per-instance omission that missed IO16 -> GPIO_17), and
  prints the drop rather than dropping it silently;
- `_proxy_rows()` fails loudly (does not silently delegate) on a
  `dispatch: cc3501e` GPIO row with no `dispatch_pin` -- a metadata
  defect, not a legitimate no-match;
- `RESERVED_CC3501E_PADS` is derived from the in-tree
  `from-cc3501e.tsv`, not a literal copied from the firmware repo;
- `_discover_targets()` finds exactly the AEN examples that enable
  CONFIG_ALP_SDK_GPIO_CC3501E_PROXY, not every cc3501e example;
- the real committed route tables never carry a reserved pad (this
  assertion was hand-verified to fail against the pre-#1859 hand-written
  table -- `{ ALP_E1M_GPIO_IO16, 17u }` -- per
  [[feedback-validate-regression-tests-against-the-broken-build]]; it is
  not re-checked against git history here so the test stays valid under a
  shallow/single-commit CI checkout);
- revision-awareness end to end: a board.yaml with `som.hw_rev: r1` (the
  AEN family's non-default rev) produces the r1 map, not r2's;
- idempotency of the real generator run -- redirected into a tmp dir via
  `_out_path_for()`, never mutating the tracked examples/ tree, and
  SKIPPED (not failed) when no clang-format is on PATH, matching
  test-all.sh's rc==99 SKIP contract.  Both matter for CI: several
  workflows (pr-metadata-validate.yml, cross-platform-zephyr.yml) run
  `pytest tests/scripts/` without installing clang-format at all, and a
  GitHub-hosted runner's default `clang-format` can be a non-22 LLVM
  build -- a test that ran the real generator unredirected would either
  hard-fail (SystemExit(99) surfacing as an error) or silently reformat
  the three committed tables with the wrong version and leave the tree
  mutated for the rest of the job.
"""

from __future__ import annotations

import importlib.util
import re
import shutil
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_cc3501e_gpio_routes.py"

EXAMPLE_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_gpio_routes.c",
)

_HAS_CLANG_FORMAT = bool(shutil.which("clang-format-22") or shutil.which("clang-format"))


@pytest.fixture(scope="module")
def gen_module():
    spec = importlib.util.spec_from_file_location("gen_cc3501e_gpio_routes", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["gen_cc3501e_gpio_routes"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_reserved_pads_derived_from_tsv_include_io16_io17_targets(gen_module):
    # The two pads every AEN example's table must never carry (#1859):
    # CC3501E GPIO_17 (E1M IO16 -> bridge READY/host-IRQ) and GPIO_16
    # (E1M IO17 -> bridge SPI0 CS).  Sourced from
    # metadata/e1m_modules/aen/from-cc3501e.tsv, not a literal mirroring
    # a firmware file that doesn't live in this repo.
    assert gen_module.RESERVED_CC3501E_PADS >= {16, 17}


def test_proxy_rows_drops_bridge_reserved_pads_and_prints_them(gen_module, capsys):
    composed = {
        "hw_rev": "r2",
        "routes": [
            {"e1m": "E1M_GPIO_IO16", "dispatch": "cc3501e", "dispatch_pin": 17,
             "som_doc": "bridge READY line -- reserved"},
            {"e1m": "E1M_GPIO_IO17", "dispatch": "cc3501e", "dispatch_pin": 16,
             "som_doc": "bridge SPI0 CS -- reserved"},
            {"e1m": "E1M_GPIO_IO9", "dispatch": "cc3501e", "dispatch_pin": 12,
             "som_doc": "ordinary proxied pad"},
            {"e1m": "E1M_GPIO_IO2", "dispatch": "direct"},
        ],
    }
    rows = gen_module._proxy_rows(composed)
    assert [e1m for e1m, _pin, _doc in rows] == ["E1M_GPIO_IO9"]
    out = capsys.readouterr().out
    assert "E1M_GPIO_IO16" in out and "GPIO_17" in out
    assert "E1M_GPIO_IO17" in out and "GPIO_16" in out


def test_proxy_rows_fails_loudly_on_cc3501e_row_missing_dispatch_pin(gen_module):
    composed = {
        "hw_rev": "r2",
        "routes": [
            {"e1m": "E1M_GPIO_IO9", "dispatch": "cc3501e"},  # no dispatch_pin: metadata bug
        ],
    }
    with pytest.raises(SystemExit) as exc:
        gen_module._proxy_rows(composed)
    assert exc.value.code != 0
    assert exc.value.code != 99  # 99 is reserved for "missing tool", not "bad data"


def test_proxy_rows_ignores_e1m_spi1_with_no_dispatch_pin(gen_module):
    """E1M_SPI1 legitimately carries dispatch: cc3501e with no
    dispatch_pin (the SoM preset's own pad_routes entry) -- must not be
    mistaken for the metadata-defect case above; the IOxx regex filters
    it out before the missing-pin check runs."""
    composed = {
        "hw_rev": "r2",
        "routes": [
            {"e1m": "E1M_SPI1", "dispatch": "cc3501e",
             "doc": "Inter-chip SPI1 fast path."},
        ],
    }
    assert gen_module._proxy_rows(composed) == []


def test_discover_targets_matches_proxy_kconfig_examples(gen_module):
    targets = {p.parent.name for p in gen_module._discover_targets()}
    assert targets == {
        "aen-cc3501e-bringup",
        "aen-cc3501e-companion-tour",
        "aen-cc3501e-gpio",
    }
    # aen-cc3501e-ble-gatt / aen-cc3501e-gatt-register also open gpio on the
    # same SoM but never set CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y -- they
    # must not get a generated table.
    assert "aen-cc3501e-ble-gatt" not in targets
    assert "aen-cc3501e-gatt-register" not in targets


_ROUTE_ENTRY_RE = re.compile(r"\{\s*ALP_(E1M_GPIO_IO\d+),\s*(\d+)u\s*\}")


def _route_pins(text: str) -> dict[str, int]:
    return {e1m: int(pin) for e1m, pin in _ROUTE_ENTRY_RE.findall(text)}


@pytest.mark.parametrize("path", EXAMPLE_ROUTE_TABLES)
def test_real_tables_never_target_a_reserved_pad(gen_module, path):
    pins = _route_pins(path.read_text(encoding="utf-8"))
    reserved_hits = {e1m: pin for e1m, pin in pins.items()
                     if pin in gen_module.RESERVED_CC3501E_PADS}
    assert not reserved_hits, (
        f"{path} routes a bridge-reserved CC3501E pad: {reserved_hits} -- "
        f"gpio_pad_reserved() would refuse this at runtime"
    )


def test_revision_dependent_pads_are_io8_io10_io21(gen_module):
    # Issue #2144: derived from metadata/e1m_modules/aen/hw-revisions.yaml
    # `pad_route_overrides:`, not hand-typed -- a future hw_rev that moves a
    # different pad must widen this set without anyone editing this test by
    # hand (mutation check: emptying _revision_dependent_e1m_pads()'s body
    # or hand-shrinking hw-revisions.yaml's r1 pad_route_overrides makes
    # this fail).
    assert gen_module.REVISION_DEPENDENT_E1M_PADS == {
        "E1M_GPIO_IO8", "E1M_GPIO_IO10", "E1M_GPIO_IO21",
    }


_REV_DEPENDENT_ARRAY_RE = re.compile(
    r"cc3501e_gpio_rev_dependent\[\]\s*=\s*\{(.*?)\};", re.DOTALL)
_REV_DEPENDENT_ENTRY_RE = re.compile(r"ALP_(E1M_GPIO_IO\d+)")


def _rev_dependent_pads(text: str) -> set[str]:
    m = _REV_DEPENDENT_ARRAY_RE.search(text)
    assert m, "cc3501e_gpio_rev_dependent[] not found in generated output"
    return set(_REV_DEPENDENT_ENTRY_RE.findall(m.group(1)))


def test_example_route_tables_no_longer_carry_a_rev_dependent_list(gen_module):
    # Issue #2144 design review: the list is identical on every AEN board,
    # so it is generated ONCE (test below), not once per app -- the
    # per-table copy this replaced was exactly the triplication issue #1859
    # already removed once for cc3501e_gpio_routes[] itself. Mutation check:
    # reverting _emit() to re-embed the block makes this fail.
    for path in EXAMPLE_ROUTE_TABLES:
        assert not _REV_DEPENDENT_ARRAY_RE.search(path.read_text(encoding="utf-8")), (
            f"{path} still carries cc3501e_gpio_rev_dependent[] -- it now "
            f"belongs solely in {gen_module.REV_DEPENDENT_OUT_PATH}"
        )


def test_sdk_owned_rev_dependent_file_carries_the_full_list(gen_module):
    # The ONE place the revision-dependent list now lives -- compiled
    # unconditionally alongside src/backends/gpio/cc3501e_proxy.c
    # (zephyr/CMakeLists.txt), never board-overridden. Mutation check:
    # deleting px_open()'s is_rev_dependent()-gate wiring in
    # src/backends/gpio/cc3501e_proxy.c doesn't touch this generated-file
    # test (that's tests/unit/gpio_cc3501e_rev_guard's job); this one fails
    # if the GENERATOR stops emitting the list or emits the wrong pads.
    text = gen_module.REV_DEPENDENT_OUT_PATH.read_text(encoding="utf-8")
    assert _rev_dependent_pads(text) == gen_module.REVISION_DEPENDENT_E1M_PADS


@pytest.mark.skipif(not _HAS_CLANG_FORMAT, reason="clang-format not on PATH")
def test_r1_board_yaml_produces_the_r1_map_not_r2s(gen_module, tmp_path):
    """End-to-end revision-awareness check: copy aen-cc3501e-gpio's
    board.yaml with `som.hw_rev: r1` added, generate against it, and
    assert the r1 map (IO8/IO10 direct, IO21 added at CC3501E GPIO_30)
    -- not r2's (IO8/IO10 proxied, no IO21).  Reproduces exactly what
    the #1859 PR review verified by hand."""
    src_board_yaml = (REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "board.yaml")
    doc = yaml.safe_load(src_board_yaml.read_text(encoding="utf-8"))
    doc["som"]["hw_rev"] = "r1"
    r1_board_yaml = tmp_path / "board.yaml"
    r1_board_yaml.write_text(yaml.safe_dump(doc), encoding="utf-8")

    composed = gen_module._composed_routes(r1_board_yaml)
    assert composed["hw_rev"] == "r1"
    rows = gen_module._proxy_rows(composed)
    pins = {e1m: pin for e1m, pin, _doc in rows}

    assert "E1M_GPIO_IO8" not in pins, "r1: IO8 is a direct Alif GPIO, not CC3501E-proxied"
    assert "E1M_GPIO_IO10" not in pins, "r1: IO10 is a direct Alif GPIO, not CC3501E-proxied"
    assert pins.get("E1M_GPIO_IO21") == 30, "r1: IO21 reaches CC3501E GPIO_30 (r2 leaves it unrouted)"
    # Everything r2 and r1 agree on stays proxied identically.
    assert pins.get("E1M_GPIO_IO9") == 12
    assert pins.get("E1M_GPIO_IO20") == 26


@pytest.mark.skipif(not _HAS_CLANG_FORMAT, reason="clang-format not on PATH")
def test_real_generation_is_idempotent(gen_module, tmp_path, monkeypatch):
    """Runs the real main() end to end, but redirected via _out_path_for()
    and _rev_dependent_out_path() into tmp_path -- the tracked examples/
    tree AND src/backends/gpio/cc3501e_rev_dependent_pins.c are never
    written to by this test (#1859 PR review: a test that mutates tracked
    files under whatever clang-format happens to be on the runner's PATH
    is worse than no test)."""
    def _redirect(app_dir: Path) -> Path:
        return tmp_path / app_dir.name / "cc3501e_gpio_routes.c"

    def _redirect_rev_dependent() -> Path:
        return tmp_path / "cc3501e_rev_dependent_pins.c"

    monkeypatch.setattr(gen_module, "_out_path_for", _redirect)
    monkeypatch.setattr(gen_module, "_rev_dependent_out_path", _redirect_rev_dependent)

    rc = gen_module.main()
    assert rc == 0
    generated = {p: p.read_bytes() for p in sorted(tmp_path.rglob("cc3501e_gpio_routes.c"))}
    assert len(generated) == 3
    rev_dependent_path = tmp_path / "cc3501e_rev_dependent_pins.c"
    assert rev_dependent_path.is_file()

    # Content must match today's committed tables (proves the redirect
    # didn't change WHAT gets generated, only WHERE).
    for path in EXAMPLE_ROUTE_TABLES:
        committed = path.read_text(encoding="utf-8")
        gen_path = tmp_path / path.parent.parent.name / "cc3501e_gpio_routes.c"
        assert gen_path.read_bytes().decode("utf-8") == committed, (
            f"generated {gen_path} differs from committed {path}"
        )
    assert rev_dependent_path.read_text(encoding="utf-8") == (
        gen_module.REV_DEPENDENT_OUT_PATH.read_text(encoding="utf-8")
    )
    rev_dependent_bytes = rev_dependent_path.read_bytes()

    rc2 = gen_module.main()
    assert rc2 == 0
    generated2 = {p: p.read_bytes() for p in sorted(tmp_path.rglob("cc3501e_gpio_routes.c"))}
    assert generated == generated2, "regenerating the same output changed it"
    assert rev_dependent_path.read_bytes() == rev_dependent_bytes, (
        "regenerating the same rev-dependent output changed it"
    )
