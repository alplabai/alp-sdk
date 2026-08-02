# SPDX-License-Identifier: Apache-2.0
"""
The hw_rev EXISTENCE gate -- issue #1025's safe half.

`scripts/alp_project_loader.py`'s `_hwrev_pad_route_overrides()` used to do

    (data.get("hw_revisions") or {}).get(hw_rev) or {}

An `hw_rev` absent from the resolved family/board table silently resolved to
empty overrides, so the build proceeded with base-revision pad routing and a
clean exit code -- a wrong-hardware emit that looked like a success.

This is deliberately NOT the `status:` question (`SdkRevisionUnsupported` /
#1019 already covers the SDK-version-range half; whether an EXISTING
`status: reserved` or `status: tbd` revision should refuse a build is a
separate, still-open question this gate does not answer -- see
`metadata/sdk_version.yaml`).  A revision that EXISTS passes here regardless
of its `status:`.

Run locally:

    python -m pytest tests/scripts/test_hw_rev_existence_gate.py -v
"""

from __future__ import annotations

import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    OrchestratorError,
    SdkRevisionUnknown,
    SdkRevisionUnsupported,
    load_board_yaml,
)
from alp_orchestrate import sdk_compat as sc  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT  # noqa: E402


# --------------------------------------------------------------------------
# The predicate itself -- pure, data-only, no board.yaml or loader involved.
# --------------------------------------------------------------------------

def test_a_known_revision_is_known():
    table = {"hw_revisions": {"r1": {}, "r2": {}}}
    assert sc.revision_known(table, "r1") is True


def test_an_unknown_revision_is_false():
    table = {"hw_revisions": {"r1": {}, "r2": {}}}
    assert sc.revision_known(table, "r99") is False


def test_a_table_with_no_hw_revisions_section_is_none_not_false():
    """Inline boards carry no per-board `hw_revisions:` table at all.

    None (not False) lets a caller skip the check entirely rather than
    refusing every inline-board build for a "revision" that was never
    declared anywhere.
    """
    assert sc.revision_known({"name": "custom-board"}, "r1") is None
    assert sc.revision_known(None, "r1") is None


def test_status_reserved_and_no_status_key_both_pass():
    """Existence is presence as a KEY -- `status:` is none of this
    predicate's business (#1025 scope line)."""
    table = {
        "hw_revisions": {
            "r3": {"status": "reserved"},
            "r4": {},  # no `status` key at all
        }
    }
    assert sc.revision_known(table, "r3") is True
    assert sc.revision_known(table, "r4") is True


@pytest.mark.parametrize("family,rev", [
    ("aen", "r2"),      # status: production
    ("aen", "r3"),      # status: reserved
    ("v2n", "r1"),      # status: production
    ("v2n", "r2"),      # no `status` key at all
    ("v2n-m1", "r2"),   # no `status` key at all
    ("imx93", "r1"),    # status: tbd, and an in-tree example builds it
])
def test_family_revision_known_passes_every_real_status_in_tree(family, rev):
    assert sc.family_revision_known(METADATA_ROOT, family, rev) is True


def test_family_revision_known_is_false_for_a_made_up_revision():
    assert sc.family_revision_known(METADATA_ROOT, "aen", "r99") is False


def test_family_available_revisions_names_the_real_aen_table():
    available = sc.family_available_revisions(METADATA_ROOT, "aen")
    assert available == sorted(available)  # deterministic order
    assert "r1" in available and "r2" in available and "r99" not in available


# --------------------------------------------------------------------------
# Reaching a caller through the orchestrator loader
# --------------------------------------------------------------------------

def _write_board(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def test_an_unknown_som_hw_rev_raises_sdk_revision_unknown(tmp_path):
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r99
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    with pytest.raises(SdkRevisionUnknown) as excinfo:
        load_board_yaml(path)

    message = str(excinfo.value)
    assert "r99" in message                 # the revision that was asked for
    assert "E1M-AEN801" in message           # which SoM
    assert "r1" in message and "r2" in message  # revisions that DO exist


def test_an_unknown_hw_rev_reports_as_unknown_not_out_of_range(tmp_path):
    """The failure must name the right cause: no bounds exist to be
    "outside" of when the revision itself was never declared."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r99
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    with pytest.raises(SdkRevisionUnknown) as excinfo:
        load_board_yaml(path)

    assert not isinstance(excinfo.value, SdkRevisionUnsupported)
    assert "does not support this hardware revision" not in str(excinfo.value)


def test_a_known_som_rev_with_an_unknown_board_rev_still_raises(tmp_path):
    """A rev present at one level (SoM: default r2) but absent at the
    other (board: e1m-evk only declares r1) resolves per the #1019
    precedent -- each side is checked independently."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
        preset: e1m-evk
        hw_rev: bogus
        cores:
          m55_hp:
            app: .
        """)

    with pytest.raises(SdkRevisionUnknown) as excinfo:
        load_board_yaml(path)

    message = str(excinfo.value)
    assert "bogus" in message
    assert "E1M-EVK" in message  # the board preset's `name:`
    assert "r1" in message


def test_a_status_reserved_som_hw_rev_resolves_cleanly(tmp_path):
    """aen r3 is `status: reserved` -- it EXISTS, so this gate passes it;
    whether reserved revisions should build is #1025's still-open half."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r3
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    project = load_board_yaml(path)
    assert project.hw_rev == "r3"


def test_a_no_status_key_som_hw_rev_resolves_cleanly(tmp_path):
    """v2n r2 carries no `status:` key at all -- still a known revision."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-V2N101
          hw_rev: r2
        preset: e1m-x-evk
        cores:
          m33_sm: {}
        """)

    project = load_board_yaml(path)
    assert project.hw_rev == "r2"


def test_the_refusal_is_an_orchestrator_error_subclass(tmp_path):
    """Existing `except OrchestratorError` handlers must keep catching it,
    exactly like `SdkRevisionUnsupported` (#1019)."""
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r99
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    with pytest.raises(OrchestratorError):
        load_board_yaml(path)


def test_the_shipped_tree_loads_cleanly():
    """The gate must not break any real, unrevisioned in-tree example."""
    example = REPO / "examples" / "peripheral-io" / "gpio-button-led" / "board.yaml"
    project = load_board_yaml(example)
    assert project.sku == "E1M-AEN801"


# --------------------------------------------------------------------------
# The `--emit composed-route-table` planner path -- alp_project_loader.py's
# OWN copy of the resolution, independent of load_board_yaml (#1025 item 5:
# "both loaders").
# --------------------------------------------------------------------------

def test_composed_route_table_also_refuses_an_unknown_hw_rev(tmp_path):
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r99
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    from alp_project_loader import _hwrev_pad_route_overrides

    with pytest.raises(SdkRevisionUnknown) as excinfo:
        _hwrev_pad_route_overrides("E1M-AEN801", "r99", METADATA_ROOT)

    message = str(excinfo.value)
    assert "r99" in message and "E1M-AEN801" in message


# --------------------------------------------------------------------------
# The exit-code contract -- `scripts/validate_board_yaml.py`
# --------------------------------------------------------------------------

def _run_validate_board_yaml(path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(REPO / "scripts" / "validate_board_yaml.py"),
         "--input", str(path), "--no-color"],
        cwd=REPO, capture_output=True, text=True, check=False,
    )


def test_validate_board_yaml_exits_with_the_new_code_not_3(tmp_path):
    path = _write_board(tmp_path, """
        som:
          sku: E1M-AEN801
          hw_rev: r99
        preset: e1m-evk
        cores:
          m55_hp:
            app: .
        """)

    proc = _run_validate_board_yaml(path)

    from validate_board_yaml import (EXIT_SDK_REVISION_UNKNOWN,
                                     EXIT_SDK_REVISION_UNSUPPORTED)
    assert EXIT_SDK_REVISION_UNKNOWN != EXIT_SDK_REVISION_UNSUPPORTED
    assert proc.returncode == EXIT_SDK_REVISION_UNKNOWN, proc.stdout + proc.stderr
    assert proc.returncode != EXIT_SDK_REVISION_UNSUPPORTED
