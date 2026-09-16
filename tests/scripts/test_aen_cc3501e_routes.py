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
    # ALP-SDK DELTA (#2051), not upstream: aen-sdcard-readout (renamed
    # aen-sdhc-probe) no longer carries its own copy -- the app dropped the
    # CC3501E bridge bring-up entirely once it stopped touching the SD mux
    # at all (sdhc0 is disabled outright on the E1M-EVK 2626-R2 now, a
    # hardware defect, not something a mux ENABLE write could route around).
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


# aen-evk-demo is a standalone app whose route table is hand-written rather
# than emitted by scripts/gen_cc3501e_gpio_routes.py: that generator
# discovers its targets by looking for a board.yaml beside a
# proxy-enabling prj.conf, and this is a standalone Zephyr app with no
# board.yaml, so it is correctly skipped. Its table is therefore pinned
# HERE instead -- against the same TSV the generator resolves through --
# so it cannot drift the way the triplicated tables #1859 removed did.
#
# ALP-SDK DELTA (#2051), not upstream: aen-sdcard-readout (renamed
# aen-sdhc-probe) used to carry the SAME single entry here (#2035, it drove
# the identical mux ENABLE) -- removed from this tuple along with its own
# cc3501e_gpio_routes.c once that app dropped the CC3501E bridge bring-up
# entirely (sdhc0 is disabled outright on the E1M-EVK 2626-R2, a hardware
# defect no mux ENABLE write could route around).
STANDALONE_APP_ROUTE_TABLES = (
    REPO / "examples" / "aen" / "aen-evk-demo" / "src" / "cc3501e_gpio_routes.c",
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


def _strip_c_comments_and_literals(text: str) -> str:
    """Blank out /* */ comments, // comments, "..." string literals and
    '...' char literals -- preserving every other character (including
    newlines) so a reported string offset still lines up with the real
    file, and so a structural check downstream can never be satisfied by
    text sitting inside a comment or a string (round-3 #2138 review: a
    prior version of this test only stripped comments, so `return
    PHASE_FAIL;` typed into a printf() format string still counted as the
    guard's return statement).

    Handles backslash escapes inside string/char literals (main.c's own
    guard message embeds `\\"%.*s\\"`) so an escaped quote does not end the
    literal early. A regex-free character scan, not a full C tokenizer --
    good enough for a structural check over one function body, not general
    C source."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "/*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif two == "//":
            end = text.find("\n", i)
            end = n if end == -1 else end
            out.append(" " * (end - i))
            i = end
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            end = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
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


def _brace_depth_at(body: str, index: int) -> int:
    """Net `{`/`}` balance in `body[:index]`, counting `body`'s OWN opening
    brace (expected at `body[0]`) as depth 1 -- so code sitting directly in
    a function's body, wrapped by nothing but the function itself, is depth
    1; code inside one more `{ ... }` (an `if`, a dead branch, ...) is depth
    2. `body` must already be comment/string-stripped (round-3 #2138
    review: a brace typed into a string or comment must never count)."""
    depth = 0
    for ch in body[:index]:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
    return depth


def _block_ends_with_bare_statement(inner: str, statement: str) -> bool:
    """True iff `statement` (e.g. "return PHASE_FAIL;") is `inner`'s OWN
    final statement -- not merely a trailing SUBSTRING, which `if (0) return
    PHASE_FAIL;` also satisfies (round-3 #2138 review mutant (e)). The
    character immediately before it, skipping whitespace, must END a prior
    statement (`;`) or OPEN this block (`{`) -- never continue one, the way
    an `if (...)` head immediately before `return` does."""
    stripped = inner.rstrip()
    if not stripped.endswith(statement):
        return False
    before = stripped[: -len(statement)].rstrip()
    return before == "" or before[-1] in "{;"


# --- examples/aen/aen-evk-demo's phase 11 IO8 guard -- pinned to its exact
# shape (round-3 #2138 review: a looser regex over the raw call text passed
# every one of 7 named mutants -- the guard wrapped in `#if 0`, in a dead
# `if (0) { ... }`, in a non-dominating `if (ctx->carrier_bus == NULL) {
# ... }`, `&& (0)` appended to the condition, a dead `if (0) return
# PHASE_FAIL;` inside the block, `return PHASE_FAIL;` sitting only inside a
# string literal, and the guard called with hardcoded `(ALP_OK,
# "2626-r2")` instead of the real variables). Pinning the exact variable
# names is deliberate, not general: this is ONE app's ONE guard, and a
# future revision-dependent pad's OWN guard gets its OWN checker in
# PAD_GUARD_CHECKERS below, not a generalisation of this one. ---

IO8_OPEN_CALL = "alp_gpio_open(ALP_E1M_GPIO_IO8)"
_IO8_GUARD_READ_RE = re.compile(r"hwrev_rc\s*=\s*alp_hw_info_read\s*\(\s*&\s*hwrev_info\s*\)\s*;")
_IO8_GUARD_IF_RE = re.compile(
    r"if\s*\(\s*!\s*aen_evkdemo_hw_rev_confirms_io8_safe\s*\(\s*hwrev_rc\s*,\s*hwrev_info\.som_hw_rev\s*\)\s*\)\s*\{"
)
# The ONE preprocessor conditional allowed between phase_sound()'s start and
# the IO8 open -- this app's own playback switch. Checked against the real
# file (main.c: the guard sits directly under this `#if`, with no `#else`/
# `#elif`/nested `#if` before the open) rather than assumed.
_IO8_ALLOWED_PP_LINE = "#if AEN_EVKDEMO_SOUND_PLAYBACK"
_PP_CONDITIONAL_RE = re.compile(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif|else)\b[^\n]*$", re.MULTILINE)


def _check_phase_sound_io8_guard(phase_sound_body: str) -> tuple[bool, str]:
    """Structural check for aen-evk-demo's phase 11 r1/r2 IO8 guard.

    `phase_sound_body` is `phase_sound()`'s `{ ... }` text (braces
    included); RAW is fine -- this strips comments/string/char literals
    itself, so it is safe to call directly with a hand-written synthetic
    body in a test, not only with text `_function_body()` already stripped.

    Returns `(True, "")` when EVERY one of these holds, in order, else
    `(False, <which one failed>)`:
      1. `alp_gpio_open(ALP_E1M_GPIO_IO8)` appears at all.
      2. No preprocessor conditional other than the one allowed
         `#if AEN_EVKDEMO_SOUND_PLAYBACK` line sits before the FIRST open.
      3. `hwrev_rc = alp_hw_info_read(&hwrev_info);` appears before the
         first open, at brace depth 1 (not nested in any block).
      4. `if (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc,
         hwrev_info.som_hw_rev)) {` appears after that, before the first
         open, ALSO at brace depth 1 -- so a copy sitting inside a dead
         `#if 0`, `if (0) { ... }`, or a branch that does not dominate the
         open (`if (ctx->carrier_bus == NULL) { ... }`) is rejected, and
         the condition itself is pinned exactly (no trailing `&& (0)`, no
         swapped-out constant arguments).
      5. That `if`'s own block -- found by a brace-depth scan, not a lazy
         "up to the first return-shaped substring" match -- closes before
         the first open, and its OWN final statement (see
         `_block_ends_with_bare_statement`) is exactly `return
         PHASE_FAIL;`.
      6. The open occurs EXACTLY ONCE (round-3 follow-up review:
         `body.find()` only ever located the first occurrence, so a
         second, later, unguarded open -- a debug re-open, say -- still
         passed). A second occurrence is rejected even when it is itself
         textually reachable only through the same guarded path -- e.g.
         straight-line code after the guard's `return PHASE_FAIL;` --
         because proving that IN GENERAL needs real control-flow
         analysis, not text-order/brace-depth checks, and this checker
         deliberately does not attempt that (see point 7).
      7. No `goto` appears anywhere in `phase_sound()` -- for the same
         reason: this checker reasons about brace nesting and text order,
         not reachability, so a `goto` jumping around the guard would
         defeat it undetected. The real file has none.
    """
    body = _strip_c_comments_and_literals(phase_sound_body)

    if re.search(r"\bgoto\b", body):
        return False, "phase_sound() contains a 'goto' -- not analysed, rejected outright"

    open_matches = list(re.finditer(re.escape(IO8_OPEN_CALL), body))
    if not open_matches:
        return False, f"{IO8_OPEN_CALL!r} not found"
    if len(open_matches) > 1:
        return False, (
            f"IO8 is opened {len(open_matches)} times in phase_sound() -- this checker "
            f"only reasons about ONE guarded open; a later occurrence is rejected even "
            f"when it looks textually dominated by the guard's return, since proving "
            f"that in general needs real control-flow analysis (see the docstring)"
        )
    first_open_idx = open_matches[0].start()
    region = body[:first_open_idx]

    pp_lines = [m.group(0).strip() for m in _PP_CONDITIONAL_RE.finditer(region)]
    bad_pp = [ln for ln in pp_lines if ln != _IO8_ALLOWED_PP_LINE]
    if bad_pp or len(pp_lines) > 1:
        return False, (
            f"unexpected preprocessor conditional(s) before the first IO8 open: {pp_lines!r} "
            f"(only one {_IO8_ALLOWED_PP_LINE!r} is allowed)"
        )

    read_match = _IO8_GUARD_READ_RE.search(region)
    if read_match is None or _brace_depth_at(body, read_match.start()) != 1:
        return False, (
            "no 'hwrev_rc = alp_hw_info_read(&hwrev_info);' at brace depth 1 before "
            "the first IO8 open"
        )

    if_match = _IO8_GUARD_IF_RE.search(region, read_match.end())
    if if_match is None:
        return False, (
            "no 'if (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, "
            "hwrev_info.som_hw_rev)) {' after the read, before the first IO8 open"
        )
    if _brace_depth_at(body, if_match.start()) != 1:
        return False, "the guard if is nested inside another block (not brace depth 1)"

    block_open = body.index("{", if_match.end() - 1)
    depth, block_close = 0, None
    for idx in range(block_open, len(body)):
        if body[idx] == "{":
            depth += 1
        elif body[idx] == "}":
            depth -= 1
            if depth == 0:
                block_close = idx
                break
    if block_close is None:
        return False, "the guard if's block never closes"
    if block_close >= first_open_idx:
        return False, "the guard if's block does not close before the first IO8 open"

    inner = body[block_open + 1:block_close]
    if not _block_ends_with_bare_statement(inner, "return PHASE_FAIL;"):
        return False, "'return PHASE_FAIL;' is not the guard block's own final statement"

    return True, ""


# --- Synthetic phase_sound()-shaped bodies proving _check_phase_sound_io8_
# guard() itself, independent of whatever main.c currently contains
# (#2138 round-3 review: "use synthetic phase_sound() bodies fed to the
# checker function, so the test doesn't edit main.c"). The first two must
# be ACCEPTED; every "mutant_*" one is a case the round-2 checker wrongly
# accepted and this one must REJECT -- the seven the review named
# (mutant_a_if_0 .. mutant_g_constant_args) plus four more from the same
# review's own mutation script for the same reason (position/removal
# mutants a lazier regex could still miss). ---

_IO8_GUARD_GOOD_BARE = (
    "{\n"
    "\talp_hw_info_t hwrev_info;\n"
    "\talp_status_t  hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
    "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
    "\t\tprintf(\"refusing\\n\");\n"
    "\t\tctx->note = \"refused: hw_rev != 2626-r2 (#2138)\";\n"
    "\t\treturn PHASE_FAIL;\n"
    "\t}\n"
    "\tmux_en = alp_gpio_open(ALP_E1M_GPIO_IO8);\n"
    "}\n"
)

# Same shape main.c actually uses: the whole thing lives under the app's
# `#if AEN_EVKDEMO_SOUND_PLAYBACK` playback switch -- proves that ONE known
# wrapper is allowed, not just a bare, unwrapped guard.
_IO8_GUARD_GOOD_WITH_PLAYBACK_WRAPPER = (
    "{\n"
    "\tif (!device_is_ready(gpio5)) {\n"
    "\t\treturn PHASE_FAIL;\n"
    "\t}\n"
    "#if AEN_EVKDEMO_SOUND_PLAYBACK\n"
    "\talp_hw_info_t hwrev_info;\n"
    "\talp_status_t  hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
    "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
    "\t\treturn PHASE_FAIL;\n"
    "\t}\n"
    "\tmux_en = alp_gpio_open(ALP_E1M_GPIO_IO8);\n"
    "#endif\n"
    "\treturn PHASE_PASS;\n"
    "}\n"
)

_IO8_GUARD_CASES: list[tuple[str, str, bool]] = [
    ("good_bare", _IO8_GUARD_GOOD_BARE, True),
    ("good_with_playback_wrapper", _IO8_GUARD_GOOD_WITH_PLAYBACK_WRAPPER, True),
    # (a) guard wrapped in a dead `#if 0` (still textually present, never compiled).
    ("mutant_a_guard_in_if_0_preprocessor", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "#if 0\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "#endif\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (b) guard wrapped in a dead `if (0) { ... }` C branch.
    ("mutant_b_guard_in_if_0_dead_branch", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (0) {\n"
        "\t\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\t\treturn PHASE_FAIL;\n"
        "\t\t}\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (c) guard wrapped in a branch that does not dominate the open.
    ("mutant_c_guard_in_nondominating_branch", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (ctx->carrier_bus == NULL) {\n"
        "\t\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\t\treturn PHASE_FAIL;\n"
        "\t\t}\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (d) `&& (0)` appended to the condition -- always false, so the `if`
    # never refuses. The old `[^{};]*` argument class swallowed `) && (0`.
    ("mutant_d_condition_and_zero", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev) && (0)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (e) a dead conditional return as the block's last statement.
    ("mutant_e_dead_conditional_return", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\tif (0) return PHASE_FAIL;\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (f) "return PHASE_FAIL;" present only inside a string literal.
    ("mutant_f_return_only_in_string_literal", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\tprintf(\"return PHASE_FAIL;\");\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # (g) hardcoded, always-safe constant arguments instead of the real vars.
    ("mutant_g_hardcoded_safe_constant_args", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(ALP_OK, \"2626-r2\")) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # Extra cases from the review's own mutation script, same reason:
    ("mutant_todo_stub", (
        "{\n"
        "\t/* TODO: add the check here */\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    ("mutant_inverted_condition", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    ("mutant_guard_moved_after_open", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "}\n"
    ), False),
    ("mutant_guard_removed_entirely", (
        "{\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    ("mutant_io8_opened_before_guard", (
        "{\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "}\n"
    ), False),
    # Round-3 follow-up review: a properly guarded FIRST open followed by a
    # second, later, unguarded one (e.g. a debug re-open) -- body.find()
    # only ever saw the first occurrence, so this passed before.
    ("mutant_second_unguarded_open_after_a_guarded_first", (
        "{\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "\tmux_en = alp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "\t/* debug re-open, added later, never re-checked */\n"
        "\tmux_en = alp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
    # Round-3 follow-up review: a goto jumping around the guard entirely.
    ("mutant_goto_around_guard", (
        "{\n"
        "\tgoto skip_guard;\n"
        "\talp_status_t hwrev_rc = alp_hw_info_read(&hwrev_info);\n"
        "\tif (!aen_evkdemo_hw_rev_confirms_io8_safe(hwrev_rc, hwrev_info.som_hw_rev)) {\n"
        "\t\treturn PHASE_FAIL;\n"
        "\t}\n"
        "skip_guard:\n"
        "\talp_gpio_open(ALP_E1M_GPIO_IO8);\n"
        "}\n"
    ), False),
]


@pytest.mark.parametrize(
    "body,expected_ok", [(c[1], c[2]) for c in _IO8_GUARD_CASES], ids=[c[0] for c in _IO8_GUARD_CASES]
)
def test_check_phase_sound_io8_guard_synthetic_cases(body, expected_ok):
    """Proves _check_phase_sound_io8_guard() itself against hand-built
    bodies, entirely independent of main.c's current content -- so this
    passes or fails on the CHECKER's own correctness, not on whatever the
    app happens to contain today (#2138 round-3 review)."""
    ok, reason = _check_phase_sound_io8_guard(body)
    assert ok == expected_ok, f"expected ok={expected_ok}, got ok={ok}, reason={reason!r}"


# Which checker function structurally proves which revision-dependent pad's
# open is guarded, per app -- e.g. _check_phase_sound_io8_guard() above for
# E1M_GPIO_IO8. A KeyError here (a guarded pad this dict has no entry for)
# is deliberate: it means a NEW revision-dependent pad showed up in a
# standalone table with no checker written for it yet, and this test
# cannot know what to require without a human writing one (#2138 round-2
# review).
PAD_GUARD_CHECKERS = {
    "E1M_GPIO_IO8": _check_phase_sound_io8_guard,
}


@pytest.mark.parametrize("path", STANDALONE_APP_ROUTE_TABLES)
def test_standalone_app_route_table_guards_revision_dependent_pads(path):
    """A hand-written route table -- no per-revision generation reaches these
    standalone apps, see the file header comment on both tables -- that maps
    a revision-dependent pad (per hw-revisions.yaml) must be paired with a
    STRUCTURAL runtime guard in the app's phase_sound(), running BEFORE that
    pad is ever opened -- see PAD_GUARD_CHECKERS' per-pad checker function
    for exactly what "structural" pins down. Without one, the table
    silently drives the wrong physical pin/chip on every revision but the
    one it was hand-built for (#2138)."""
    revision_dependent = _revision_dependent_e1m_pads()
    routes = _example_gpio_routes(path)
    guarded_pads = revision_dependent & routes.keys()
    if not guarded_pads:
        pytest.skip(f"{path.name} routes no revision-dependent pad")

    main_c_path = path.parent / "main.c"
    main_c_raw = main_c_path.read_text(encoding="utf-8")
    main_c_nocomments = _strip_c_comments_and_literals(main_c_raw)
    phase_sound_body = _function_body(
        main_c_nocomments,
        r"static phase_verdict_t phase_sound\(demo_ctx_t \*ctx\)\s*\{",
        main_c_path,
    )

    for e1m in sorted(guarded_pads):
        checker = PAD_GUARD_CHECKERS[e1m]
        ok, reason = checker(phase_sound_body)
        assert ok, (
            f"{main_c_path}: {e1m} is revision-dependent "
            f"(metadata/e1m_modules/aen/hw-revisions.yaml pad_route_overrides) "
            f"but its phase_sound() guard is not structurally sound: {reason} -- "
            f"on r1 this pad routes to a DIFFERENT physical pin/chip than the "
            f"hand-written table assumes (#2138)"
        )
