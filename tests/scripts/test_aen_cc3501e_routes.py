# SPDX-License-Identifier: Apache-2.0
"""Regression tests for AEN CC3501E GPIO routes and bridge helpers."""
from __future__ import annotations

import csv
import json
import re
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
METADATA = REPO / "metadata"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"

AEN_SKUS = (
    "E1M-AEN301",
    "E1M-AEN401",
    "E1M-AEN501",
    "E1M-AEN601",
    "E1M-AEN701",
    "E1M-AEN801",
)

EXAMPLE_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_gpio_routes.c",
)

EXAMPLE_BRIDGE_HELPERS = (
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-companion-tour" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-cc3501e-gpio" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-usb-firstlight" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-usb-firstlight" / "src" / "cc3501e_bridge.h",
    REPO / "examples" / "aen" / "aen-sdcard-readout" / "src" / "cc3501e_bridge.c",
    REPO / "examples" / "aen" / "aen-sdcard-readout" / "src" / "cc3501e_bridge.h",
)


def _tsv_gpio_routes() -> dict[str, int]:
    """Return E1M GPIO pad -> raw CC3501E GPIO from the AEN TSV source."""
    path = METADATA / "e1m_modules" / "aen" / "from-cc3501e.tsv"
    routes: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as f:
        rows = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            e1m_match = re.fullmatch(r"IO(\d+)", row["e1m_function"])
            if not e1m_match:
                continue
            # A raw-GPIO row is 3 columns and carries the pad in
            # cc3501e_function; a row whose pad is claimed by a named
            # peripheral is 4 columns, with the claim in cc3501e_function and
            # the pad in cc3501e_pad.  IO16 and IO17 took the second shape when
            # the bridge's READY and SPI CSN claims were recorded (#1808), so
            # read the pad from whichever column actually holds one instead of
            # assuming the 3-column layout.
            for cell in (row["cc3501e_function"], row["cc3501e_pad"]):
                gpio_match = re.fullmatch(r"GPIO_?(\d+)", cell or "")
                if gpio_match:
                    routes[f"E1M_GPIO_IO{e1m_match.group(1)}"] = int(
                        gpio_match.group(1))
                    break
    return routes


def _sku_gpio_routes(sku: str) -> dict[str, int]:
    """Return CC3501E GPIO pad_routes from one AEN SoM preset."""
    path = METADATA / "e1m_modules" / f"{sku}.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    routes: dict[str, int] = {}
    for row in doc["pad_routes"]:
        e1m = row["e1m"]
        if row.get("dispatch") == "cc3501e" and e1m.startswith("E1M_GPIO_IO"):
            routes[e1m] = int(row["dispatch_pin"])
    return routes


def _example_gpio_routes(path: Path) -> dict[str, int]:
    """Return the strong cc3501e_gpio_routes[] entries from an example."""
    text = path.read_text(encoding="utf-8")
    routes: dict[str, int] = {}
    for e1m, pin in re.findall(r"\{\s*ALP_(E1M_GPIO_IO\d+),\s*(\d+)u\s*\}", text):
        routes[e1m] = int(pin)
    return routes


def _tsv_reserved_pads() -> set[int]:
    """CC3501E GPIO indices reserved from the host GPIO proxy: a TSV row
    whose e1m_function is an IOxx pad and whose cc3501e_function is a
    NAMED claim (BRIDGE_READY, BRIDGE_SPI_CSN -- #1808's peripheral-column
    convention) rather than a bare GPIOxx pad name.  Independently coded
    from scripts/gen_cc3501e_gpio_routes.py's own
    _reserved_pads_from_tsv() (both read the same TSV, but via separate
    parsing) so this test is a real cross-check, not a tautology against
    the generator's own logic."""
    path = METADATA / "e1m_modules" / "aen" / "from-cc3501e.tsv"
    reserved: set[int] = set()
    with path.open(newline="", encoding="utf-8") as f:
        rows = (line for line in f if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            if not re.fullmatch(r"IO\d+", row["e1m_function"]):
                continue
            if re.fullmatch(r"GPIO_?\d+", row["cc3501e_function"] or ""):
                continue  # unclaimed -- an ordinary proxyable pad
            pad_match = re.fullmatch(r"GPIO_?(\d+)", row["cc3501e_pad"] or "")
            if pad_match:
                reserved.add(int(pad_match.group(1)))
    return reserved


def _composed_cc3501e_gpio_routes(board_yaml: Path) -> dict[str, int]:
    """Resolve <board_yaml>'s OWN composed-route-table (hw_rev-aware,
    scripts/alp_project.py --emit composed-route-table) and return its
    CC3501E-dispatched, non-reserved GPIO pads -- what
    scripts/gen_cc3501e_gpio_routes.py should have written for exactly
    this board.yaml.  Unlike a plain from-cc3501e.tsv read, this
    correctly differs for a board.yaml that sets som.hw_rev (#1859 PR
    review: a `som.hw_rev: r1` board must NOT be compared against r2's
    TSV-only expectation)."""
    proc = subprocess.run(
        [sys.executable, str(ALP_PROJECT), "--input", str(board_yaml),
         "--emit", "composed-route-table"],
        capture_output=True, text=True, check=True,
    )
    composed = json.loads(proc.stdout)
    reserved = _tsv_reserved_pads()
    routes: dict[str, int] = {}
    for row in composed["routes"]:
        if row.get("dispatch") != "cc3501e":
            continue
        e1m = row.get("e1m", "")
        if not re.fullmatch(r"E1M_GPIO_IO\d+", e1m):
            continue
        pin = row.get("dispatch_pin")
        if pin is None or int(pin) in reserved:
            continue
        routes[e1m] = int(pin)
    return routes


def test_tsv_captures_io9_and_io16_io17_crossing():
    routes = _tsv_gpio_routes()
    assert routes["E1M_GPIO_IO9"] == 12
    assert routes["E1M_GPIO_IO16"] == 17
    assert routes["E1M_GPIO_IO17"] == 16


@pytest.mark.parametrize("sku", AEN_SKUS)
def test_aen_som_gpio_pad_routes_match_tsv_source(sku):
    assert _sku_gpio_routes(sku) == _tsv_gpio_routes()


@pytest.mark.parametrize("path", EXAMPLE_ROUTE_TABLES)
def test_example_route_tables_match_som_metadata_subset(path):
    # Built from THIS example's own board.yaml, hw_rev included -- not
    # from-cc3501e.tsv directly, which is production-rev (r2) only and
    # would wrongly redden this test the moment any example declares
    # `som.hw_rev: r1` (#1859 PR review: this previously hardcoded the
    # TSV's r2 set minus IO16/IO17, so it could never validate the PR's
    # own headline feature -- per-example revision-awareness).
    board_yaml = path.parent.parent / "board.yaml"
    expected = _composed_cc3501e_gpio_routes(board_yaml)

    assert _example_gpio_routes(path) == expected


@pytest.mark.parametrize("suffix", ("cc3501e_bridge.c", "cc3501e_bridge.h"))
def test_example_bridge_helpers_stay_in_sync(suffix):
    reference = (
        REPO / "examples" / "aen" / "aen-cc3501e-bringup" / "src" / suffix
    ).read_text(encoding="utf-8")
    for path in EXAMPLE_BRIDGE_HELPERS:
        if path.name == suffix:
            assert path.read_text(encoding="utf-8") == reference, f"{path} drifted"


# aen-evk-demo and aen-sdcard-readout are the standalone apps whose route
# table is hand-written rather than emitted by
# scripts/gen_cc3501e_gpio_routes.py: that generator discovers its targets by
# looking for a board.yaml beside a proxy-enabling prj.conf, and both are
# standalone Zephyr apps with no board.yaml, so both are correctly skipped.
# Their tables are therefore pinned HERE instead -- against the same TSV the
# generator resolves through -- so they cannot drift the way the triplicated
# tables #1859 removed did. aen-sdcard-readout's table (#2035) declares the
# SAME single entry as aen-evk-demo's (it drives the identical mux ENABLE),
# so both are checked by the same two assertions below.
STANDALONE_APP_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-evk-demo" / "src" / "cc3501e_gpio_routes.c",
    REPO / "examples" / "aen" / "aen-sdcard-readout" / "src" / "cc3501e_gpio_routes.c",
)


@pytest.mark.parametrize("path", STANDALONE_APP_ROUTE_TABLES)
def test_standalone_app_route_table_is_a_metadata_backed_subset(path):
    routes = _example_gpio_routes(path)
    tsv = _tsv_gpio_routes()

    # SUBSET, not equality: each app declares only the pads it actually
    # drives (the SDIO mux ENABLE), so a full-map comparison would be wrong.
    # Every entry it does declare must match metadata exactly.
    assert routes, f"{path} declares no proxied route at all"
    assert routes.items() <= tsv.items(), (
        f"{path}'s hand-written route table disagrees with "
        f"from-cc3501e.tsv: {routes} vs {tsv}"
    )

    # The mux ENABLE is the reason the table exists; naming it explicitly
    # means deleting the entry fails here rather than silently turning
    # alp_gpio_open(IO20) into a write to an unconnected Alif pad.
    assert routes.get("E1M_GPIO_IO20") == 26

    # IO21 -- the mux SELECT -- must NEVER appear.  It is physically open on
    # r2, and on r1 driving it would contend with a fitted P18 jumper on the
    # shared MUX_SEL.SDIO net.  See the app's main.c mux-enable comment.
    assert "E1M_GPIO_IO21" not in routes


@pytest.mark.parametrize("path", STANDALONE_APP_ROUTE_TABLES)
def test_standalone_app_route_table_never_targets_a_reserved_pad(path):
    reserved = _tsv_reserved_pads()
    hits = {
        e1m: pin
        for e1m, pin in _example_gpio_routes(path).items()
        if pin in reserved
    }
    assert not hits, (
        f"{path} routes a bridge-reserved CC3501E pad: {hits} -- "
        f"the firmware's gpio_pad_reserved() would refuse it at runtime"
    )


def _revision_dependent_e1m_pads() -> set[str]:
    """E1M pads that metadata/e1m_modules/aen/hw-revisions.yaml moves between
    chips on at least one AEN hw_rev -- IO8/IO10/IO21 today, via r1's
    `pad_route_overrides`. A route-table entry for one of these pads is only
    ever correct on the ONE revision its target pin/chip was resolved for --
    on any other revision it silently drives a DIFFERENT physical pin/chip
    (issue #2138: aen-evk-demo's hand-written IO8 -> CC3501E GPIO_30 entry is
    correct on r2 only; on r1 GPIO_30 is the carrier's SDIO mux SELECT, tied
    to +3V3 by a fitted P18 jumper)."""
    path = METADATA / "e1m_modules" / "aen" / "hw-revisions.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    pads: set[str] = set()
    for rev in (doc.get("hw_revisions") or {}).values():
        for row in rev.get("pad_route_overrides") or []:
            pads.add(row["e1m"])
    return pads


# Which pure verdict function gates which revision-dependent pad's open, per
# app -- e.g. examples/aen/aen-evk-demo/src/hw_rev_verdict.h's
# aen_evkdemo_hw_rev_confirms_io8_safe() for E1M_GPIO_IO8. A KeyError here
# (a guarded pad this dict has no entry for) is deliberate: it means a NEW
# revision-dependent pad showed up in a standalone table with no guard
# function named for it yet, and this test cannot know what to require
# without a human naming it (#2138 round-2 review).
PAD_GUARD_FUNCTIONS = {
    "E1M_GPIO_IO8": "aen_evkdemo_hw_rev_confirms_io8_safe",
}


def _strip_c_comments(text: str) -> str:
    """Blank out /* */ and // comments, preserving every other character
    (including newlines) so a reported string offset still lines up with
    the real file. A regex-free character scan, not a full C tokenizer --
    good enough for a structural check over one function body, not general
    C source (a string literal containing "/*" would fool it; the guard
    region this test scans over contains none)."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        if text[i:i + 2] == "/*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text[i:i + 2] == "//":
            end = text.find("\n", i)
            end = n if end == -1 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _function_body(text: str, signature_re: str, main_c_path: Path) -> str:
    """Return `text[start:end]` (braces included) for the FIRST function
    whose signature matches `signature_re`, found via a brace-depth scan
    rather than a single closing-brace regex -- phase_sound() itself
    contains nested `{ ... }` blocks (the mux-enable guard, the AMP_ENABLE
    reset, the per-amp loops), so a naive "up to the next line starting
    with `}`" match would return early on the FIRST of those, not the
    function's own end."""
    m = re.search(signature_re, text)
    assert m, f"{main_c_path}: could not find a function matching {signature_re!r}"
    depth = 0
    start = text.index("{", m.end() - 1)
    for idx in range(start, len(text)):
        if text[idx] == "{":
            depth += 1
        elif text[idx] == "}":
            depth -= 1
            if depth == 0:
                return text[start:idx + 1]
    raise AssertionError(f"{main_c_path}: function body starting at offset {start} never closes")


@pytest.mark.parametrize("path", STANDALONE_APP_ROUTE_TABLES)
def test_standalone_app_route_table_guards_revision_dependent_pads(path):
    """A hand-written route table -- no per-revision generation reaches these
    standalone apps, see the file header comment on both tables -- that maps
    a revision-dependent pad (per hw-revisions.yaml) must be paired with a
    runtime `if (!<guard>(...)) { ... return PHASE_FAIL; }` in the app's
    phase_sound(), running BEFORE that pad is ever opened. Without one, the
    table silently drives the wrong physical pin/chip on every revision but
    the one it was hand-built for (#2138).

    This intentionally checks the STRUCTURE of the guard, not just that its
    name appears somewhere in the file: the round-2 review of #2138 found
    that a plain substring search passes both a `/* TODO: add the check
    here */` stub in place of the guard and the guard's own condition
    inverted (`if (guard_ok)` instead of `if (!guard_ok)`, which refuses r2
    and drives the pin on r1 -- the exact short this issue exists to
    prevent). Stripping comments first closes the stub case (nothing left
    for the regex to find); requiring the literal `!` before the call
    closes the inversion case."""
    revision_dependent = _revision_dependent_e1m_pads()
    routes = _example_gpio_routes(path)
    guarded_pads = revision_dependent & routes.keys()
    if not guarded_pads:
        pytest.skip(f"{path.name} routes no revision-dependent pad")

    main_c_path = path.parent / "main.c"
    main_c_raw = main_c_path.read_text(encoding="utf-8")
    main_c_nocomments = _strip_c_comments(main_c_raw)
    phase_sound_body = _function_body(
        main_c_nocomments,
        r"static phase_verdict_t phase_sound\(demo_ctx_t \*ctx\)\s*\{",
        main_c_path,
    )

    for e1m in sorted(guarded_pads):
        guard_fn = PAD_GUARD_FUNCTIONS[e1m]
        open_call = f"alp_gpio_open(ALP_{e1m})"
        open_idx = phase_sound_body.find(open_call)
        assert open_idx != -1, (
            f"{path} routes revision-dependent pad {e1m} but {main_c_path}'s "
            f"phase_sound() never calls {open_call} -- update this test if "
            f"the app was rewritten to open it another way"
        )

        guard_re = re.compile(
            r"if\s*\(\s*!\s*" + re.escape(guard_fn) + r"\s*\([^{};]*\)\s*\)\s*\{"
            r"[^{}]*?return\s+PHASE_FAIL\s*;",
            re.DOTALL,
        )
        guard_match = guard_re.search(phase_sound_body)
        assert guard_match and guard_match.start() < open_idx, (
            f"{main_c_path}: {e1m} is revision-dependent "
            f"(metadata/e1m_modules/aen/hw-revisions.yaml pad_route_overrides) "
            f"but phase_sound() opens it ({open_call}) with no "
            f"'if (!{guard_fn}(...)) {{ ... return PHASE_FAIL; }}' guard "
            f"(comment-stripped, structure-checked) running before it -- on "
            f"r1 this pad routes to a DIFFERENT physical pin/chip than the "
            f"hand-written table assumes (#2138)"
        )
