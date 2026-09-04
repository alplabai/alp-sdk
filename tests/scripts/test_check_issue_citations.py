"""Unit tests for scripts/check_issue_citations.py (#1950).

The load-bearing case (`test_gd32g553_repro_...`) reproduces the real
`metadata/chips/gd32g553.yaml:13-16` defect verbatim: three closed issues
(#494/#495/#496) cited in the `driver_status: partial` comment as the open
reasons the driver isn't complete.
"""

import datetime
import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_issue_citations.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_issue_citations", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _chip_yaml(root: Path, chip_id: str, body: str):
    chips = root / "metadata" / "chips"
    chips.mkdir(parents=True, exist_ok=True)
    (chips / f"{chip_id}.yaml").write_text(body, encoding="utf-8")


def _snapshot(root: Path, issues: dict, *, age_days: float = 0.0):
    ts = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(days=age_days)
    doc = {
        "generated_at": ts.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "issues": issues,
    }
    snap = root / "metadata" / "issue-state-snapshot.json"
    snap.parent.mkdir(parents=True, exist_ok=True)
    snap.write_text(json.dumps(doc), encoding="utf-8")
    return snap


# -- clean tree ---------------------------------------------------------------


def test_clean_tree_passes(tmp_path):
    mod = _load()
    _chip_yaml(
        tmp_path,
        "widget",
        "chip_id: widget\ndriver_status: complete    # init + read, no open issues.\n",
    )
    _snapshot(tmp_path, {})
    assert mod.find_problems(tmp_path) == []


def test_no_metadata_dir_passes(tmp_path):
    mod = _load()
    _snapshot(tmp_path, {})
    assert mod.find_problems(tmp_path) == []


# -- the real defect (#1950) ---------------------------------------------------


_GD32G553_BLOCK = (
    "chip_id:          gd32g553\n"
    "driver_status:    partial               # host driver: full surface.\n"
    "                                        # firmware bodies landed. Still\n"
    "                                        # partial: PWM_CONFIGURE +\n"
    "                                        # ADC_CONFIGURE only accept\n"
    "                                        # defaults (#495 / #494); ADC DSP\n"
    "                                        # chains bind but don't yet\n"
    "                                        # transform stream samples (#496);\n"
    "                                        # PWM_CAPTURE edges pending a V2N\n"
    "                                        # pad-routing rework.\n"
    "display_name:     \"gd32g553\"\n"
)


def test_gd32g553_repro_three_closed_issues_flagged_as_open_blockers(tmp_path):
    """MUST fail: the real 2026-09-04 gd32g553.yaml shape, all three cited
    issues closed months earlier, per the issue text."""
    mod = _load()
    _chip_yaml(tmp_path, "gd32g553", _GD32G553_BLOCK)
    _snapshot(tmp_path, {"494": "CLOSED", "495": "CLOSED", "496": "CLOSED"})

    problems = mod.find_problems(tmp_path)
    assert len(problems) == 3, problems
    joined = "\n".join(problems)
    assert "#494" in joined and "CLOSED" in joined
    assert "#495" in joined
    assert "#496" in joined
    assert all("gd32g553.yaml:2" in p for p in problems)


def test_gd32g553_repro_still_open_issue_not_flagged(tmp_path):
    """Non-vacuity: if the cited issues are genuinely still open, no finding."""
    mod = _load()
    _chip_yaml(tmp_path, "gd32g553", _GD32G553_BLOCK)
    _snapshot(tmp_path, {"494": "OPEN", "495": "OPEN", "496": "OPEN"})
    assert mod.find_problems(tmp_path) == []


# -- historical vs. blocker phrasing -------------------------------------------


def test_historical_via_citation_is_not_flagged(tmp_path):
    """The corrected #1949 wording ('#494 ... via #730') must not trip the
    gate once it lands -- 'via' marks a citation as historical, not a
    blocker claim."""
    mod = _load()
    _chip_yaml(
        tmp_path,
        "gd32g553",
        "chip_id: gd32g553\n"
        "driver_status:    partial   # Still partial, for reasons that are NOT\n"
        "                            # open issues -- #494, #495 and #496 all\n"
        "                            # closed (#494/#495 via #730, #496 via\n"
        "                            # #764).  What remains: pad routing.\n",
    )
    _snapshot(
        tmp_path,
        {"494": "CLOSED", "495": "CLOSED", "496": "CLOSED",
         "730": "MERGED", "764": "MERGED"},
    )
    assert mod.find_problems(tmp_path) == []


def test_landed_phrasing_is_not_flagged(tmp_path):
    mod = _load()
    _chip_yaml(
        tmp_path,
        "pca9451a",
        "chip_id: pca9451a\n"
        "driver_status:    partial   # probe + raw R/W + per-rail voltage\n"
        "                            # (mV) on all 6 bucks + 5 LDOs now land\n"
        "                            # (issue #474).\n",
    )
    _snapshot(tmp_path, {"474": "CLOSED"})
    assert mod.find_problems(tmp_path) == []


# -- staleness degrades to WARNING, never a hard fail (#1950 design decision) --


def test_stale_snapshot_degrades_to_warning_not_error(tmp_path):
    mod = _load()
    _chip_yaml(tmp_path, "gd32g553", _GD32G553_BLOCK)
    _snapshot(tmp_path, {"494": "CLOSED", "495": "CLOSED", "496": "CLOSED"},
              age_days=mod._STALE_AFTER_DAYS + 1)

    assert mod.find_problems(tmp_path) == []
    warnings = mod.find_warnings(tmp_path)
    assert any("stale" in w for w in warnings)
    assert any("#494" in w for w in warnings)


def test_missing_snapshot_degrades_to_warning_not_error(tmp_path):
    mod = _load()
    _chip_yaml(tmp_path, "gd32g553", _GD32G553_BLOCK)
    # No snapshot file written at all.
    assert mod.find_problems(tmp_path) == []
    warnings = mod.find_warnings(tmp_path)
    assert any("no issue-state snapshot" in w for w in warnings)


def test_citation_unknown_to_snapshot_is_warning_not_error(tmp_path):
    mod = _load()
    _chip_yaml(tmp_path, "gd32g553", _GD32G553_BLOCK)
    _snapshot(tmp_path, {})  # fresh, but doesn't know these numbers

    assert mod.find_problems(tmp_path) == []
    warnings = mod.find_warnings(tmp_path)
    assert any("no record of it" in w for w in warnings)


# -- header @par Driver status: blocks -----------------------------------------


def test_header_status_block_citation_flagged(tmp_path):
    mod = _load()
    hdr_dir = tmp_path / "include" / "alp" / "chips"
    hdr_dir.mkdir(parents=True)
    (hdr_dir / "widget.h").write_text(
        "/**\n"
        " * @par Driver status: PARTIAL\n"
        " *\n"
        " * Blocked on #999 for the rest of the register map.\n"
        " *\n"
        " * @par Pin model\n"
        " */\n",
        encoding="utf-8",
    )
    _snapshot(tmp_path, {"999": "CLOSED"})
    problems = mod.find_problems(tmp_path)
    assert len(problems) == 1
    assert "#999" in problems[0]
    assert "widget.h" in problems[0]


def test_header_block_stops_at_next_par_tag(tmp_path):
    """A citation AFTER the next @par tag is out of scope -- it belongs to
    a different paragraph, not the driver-status claim."""
    mod = _load()
    hdr_dir = tmp_path / "include" / "alp" / "chips"
    hdr_dir.mkdir(parents=True)
    (hdr_dir / "widget.h").write_text(
        "/**\n"
        " * @par Driver status: COMPLETE\n"
        " *\n"
        " * @par See also\n"
        " *\n"
        " * Historical note, see #999.\n"
        " */\n",
        encoding="utf-8",
    )
    _snapshot(tmp_path, {"999": "CLOSED"})
    assert mod.find_problems(tmp_path) == []
