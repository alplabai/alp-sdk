# SPDX-License-Identifier: Apache-2.0
"""An UNREADABLE SoM-family hw-revisions.yaml must REFUSE, not fail open.

alplabai/tan-cli#563.  All three hw_rev safety gates -- the unknown-revision
gate (#1025's existence half), the not-buildable gate (#1025's status half)
and the SDK-version-range gate (#1019) -- read exactly one file per SoM
family, `metadata/e1m_modules/<family>/hw-revisions.yaml`, through
`sdk_compat.load_family_table()`.

That reader swallowed `OSError` and `yaml.YAMLError` and answered `{}`.
Every predicate above it reads an empty table as "nothing to judge" and
returns None, so ONE tab-indented line in that file silently disabled all
three gates at once and `load_board_yaml()` returned a project for an
unverified hw_rev at exit 0 -- a wrong-hardware artefact that looks like a
success.  These tests pin the refusal.

An ABSENT table stays benign and is pinned here too: an in-development
family that ships no table has nothing to check against, and
`tests/scripts/_orchestrate_support.py`'s NX9101 fixture depends on it.

TWO independent readers open this one file in alp-sdk today, and both fell
open on at least two of the four unusable shapes.  Each has its own section
below:

  1. `sdk_compat.load_family_table()` -- the three gates above.
  2. `alp_project_loader._hwrev_pad_route_overrides()` -- the
     `--emit composed-route-table` / `--emit carrier-netlist` path, which
     resolves its SoM data independently and carries its own copies of the
     #1025 gates.  Its `yaml.safe_load(...) or {}` fell open on an empty
     and on a truncated table.

Both now read through the one guarded reader, so they cannot disagree about
whether a damaged table is fatal or about what the refusal says.

A third reader, `alp_cli.new_som._family_hw_revisions()` -- the scaffold-time
`--default-hw-rev` cross-check, which fell open widest of the three -- was
covered here too until `scripts/alp_cli/new_som.py` retired alongside the
rest of the alp_cli command surface (alp-sdk#1368). `tan new-som` is its
sole surviving front door now (ADR-0020 end-state B); it ported the same
guard natively (alplabai/tan-cli#563, `python/tan/commands/new_som_cmd.py`)
with its own tan-cli test coverage, independent of this file.

Run locally:

    python -m pytest tests/scripts/test_hw_rev_table_unreadable.py -v
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    OrchestratorError,
    SdkRevisionNotBuildable,
    SdkRevisionUnknown,
    SdkRevisionUnsupported,
    load_board_yaml,
)
from alp_orchestrate import sdk_compat as sc  # noqa: E402
from alp_project_loader import _hwrev_pad_route_overrides  # noqa: E402

# One tab-indented line -- the plausible hand-edit tan-cli#563 names, and
# the one YAML rejects outright ("found character '\t' that cannot start
# any token").
_TAB_INDENTED = "family: aen\nhw_revisions:\n\tr1:\n\t\tstatus: production\n"


def _metadata_copy(tmp_path: Path) -> Path:
    dest = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", dest)
    return dest


def _family_table(meta: Path, family: str) -> Path:
    return meta / "e1m_modules" / family / "hw-revisions.yaml"


def _board(tmp_path: Path, body: str) -> Path:
    (tmp_path / "src").mkdir(exist_ok=True)
    path = tmp_path / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


# The three board.yaml files, one per gate, each of which the INTACT tree
# refuses.  Every one must keep refusing when the table it is checked
# against is unreadable -- with a different error, but never with a pass.
_AEN_UNKNOWN_REV = """
    som:
      sku: E1M-AEN801
      hw_rev: r99
    preset: e1m-evk
    cores:
      m55_hp:
        app: ./src
"""

_NX9101_NOT_BUILDABLE = """
    som:
      sku: E1M-NX9101
      hw_rev: r1
    preset: e1m-evk
    cores:
      a55_cluster:
        os: yocto
        image: alp-image-edge
"""

_AEN_IN_RANGE = """
    som:
      sku: E1M-AEN801
      hw_rev: r2
    preset: e1m-evk
    cores:
      m55_hp:
        app: ./src
"""


# --------------------------------------------------------------------------
# The reader itself
# --------------------------------------------------------------------------

def test_an_absent_family_table_is_still_benign(tmp_path):
    """An ABSENT table is the one case `{}` is the right answer for.

    A family with no `hw-revisions.yaml` has nothing to check against, so
    the tri-state predicates answer None and the gates skip.  This is the
    behaviour `_orchestrate_support._synthetic_nx9101_root` relies on;
    the #563 fix must not take it away.
    """
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").unlink()

    assert sc.family_revision_known(meta, "aen", "r99") is None
    assert sc.family_revision_buildable(meta, "aen", "r99") is None
    assert sc.family_available_revisions(meta, "aen") == []
    assert sc.family_revision(meta, "aen", "r1") == {}


@pytest.mark.parametrize("content,why", [
    (_TAB_INDENTED,                      "not valid YAML"),
    ("",                                 "an empty file"),
    ("just a bare scalar\n",             "does not parse to a mapping"),
    ("family: aen\ndisplay_name: AEN\n", "no `hw_revisions:` mapping"),
])
def test_a_present_but_unusable_table_refuses(tmp_path, content, why):
    """Present-but-unusable is the OPPOSITE of absent: there IS something
    to check against and we failed to read it.

    Covers the YAML parse error #563 reports plus the shapes that parse
    without raising and disable the gates just as completely -- an empty
    file (`None`), a scalar, and a file truncated above its
    `hw_revisions:` block.
    """
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").write_text(content, encoding="utf-8")

    with pytest.raises(OrchestratorError) as excinfo:
        sc.family_revision_known(meta, "aen", "r1")

    message = str(excinfo.value)
    assert "hw-revisions.yaml" in message      # names the file
    assert why in message                      # names what is wrong with it


def test_an_unreadable_table_refuses_every_reader(tmp_path):
    """Not just `family_revision_known` -- every helper that reads the
    family table refuses, so no gate can reach a "nothing to judge" None
    by taking a different door into the same file."""
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").write_text(_TAB_INDENTED, encoding="utf-8")

    for call in (
        lambda: sc.family_revision(meta, "aen", "r1"),
        lambda: sc.family_revision_known(meta, "aen", "r1"),
        lambda: sc.family_revision_buildable(meta, "aen", "r1"),
        lambda: sc.family_available_revisions(meta, "aen"),
    ):
        with pytest.raises(OrchestratorError):
            call()


# --------------------------------------------------------------------------
# The three gates, through the loader -- the wrong-hardware emit itself
# --------------------------------------------------------------------------

def test_gate_1_unknown_revision_does_not_fail_open(tmp_path):
    """Intact tree: SdkRevisionUnknown.  Corrupt table: still a refusal.

    Before #563 this returned a BoardProject with hw_rev='r99' at exit 0.
    """
    intact = _metadata_copy(tmp_path / "intact")
    board = _board(tmp_path, _AEN_UNKNOWN_REV)
    with pytest.raises(SdkRevisionUnknown):
        load_board_yaml(board, metadata_root=intact)

    broken = _metadata_copy(tmp_path / "broken")
    _family_table(broken, "aen").write_text(_TAB_INDENTED, encoding="utf-8")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(board, metadata_root=broken)
    assert "hw-revisions.yaml" in str(excinfo.value)


def test_gate_2_not_buildable_revision_does_not_fail_open(tmp_path):
    """imx93 r1 is `status: tbd`.  A corrupt imx93 table must not turn
    that into a buildable revision."""
    intact = _metadata_copy(tmp_path / "intact")
    board = _board(tmp_path, _NX9101_NOT_BUILDABLE)
    with pytest.raises(SdkRevisionNotBuildable):
        load_board_yaml(board, metadata_root=intact)

    broken = _metadata_copy(tmp_path / "broken")
    _family_table(broken, "imx93").write_text(_TAB_INDENTED, encoding="utf-8")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(board, metadata_root=broken)
    assert "hw-revisions.yaml" in str(excinfo.value)


def test_gate_3_sdk_version_range_does_not_fail_open(tmp_path):
    """The #1019 range gate reads the same file, so it fails open the
    same way.  Cap aen r2 below the running SDK to arm it."""
    def cap(meta: Path) -> None:
        path = _family_table(meta, "aen")
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        doc["hw_revisions"]["r2"]["max_sdk_version"] = "0.5.0"
        path.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")

    intact = _metadata_copy(tmp_path / "intact")
    cap(intact)
    board = _board(tmp_path, _AEN_IN_RANGE)
    with pytest.raises(SdkRevisionUnsupported):
        load_board_yaml(board, metadata_root=intact)

    broken = _metadata_copy(tmp_path / "broken")
    cap(broken)
    _family_table(broken, "aen").write_text(_TAB_INDENTED, encoding="utf-8")
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(board, metadata_root=broken)
    assert "hw-revisions.yaml" in str(excinfo.value)


def test_the_refusal_is_catchable_as_orchestrator_error(tmp_path):
    """Not one of the `SdkRevision*` subclasses on purpose: those name a
    defect in the customer's board.yaml and `scripts/validate_board_yaml.py`
    maps each to its own exit code.  An unreadable metadata table is a
    defect in the SDK's own tree, so it takes the generic refusal."""
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").write_text(_TAB_INDENTED, encoding="utf-8")

    with pytest.raises(OrchestratorError) as excinfo:
        sc.family_revision_known(meta, "aen", "r1")
    assert not isinstance(excinfo.value,
                          (SdkRevisionUnknown, SdkRevisionNotBuildable,
                           SdkRevisionUnsupported))


def test_the_shipped_tree_still_loads_cleanly():
    """Negative control: the real metadata tree parses, so none of the
    new refusals fire on it."""
    project = load_board_yaml(
        REPO / "examples" / "bringup" / "board-selftest" / "board.yaml")
    assert project.sku == "E1M-AEN801"


# --------------------------------------------------------------------------
# Reader 2: the composed-route-table / carrier-netlist emit path
#
# `alp_project_loader._hwrev_pad_route_overrides()` resolves its own SoM
# data independently of `alp_orchestrate.loader`, so it carries its own
# copies of the #1025 existence and buildable gates -- and it had its own
# `yaml.safe_load(...) or {}`, which fell open on the two shapes that parse
# without raising.  It now reads through the same guarded reader.
# --------------------------------------------------------------------------

@pytest.mark.parametrize("content", [
    _TAB_INDENTED,
    "",
    "just a bare scalar\n",
    "family: aen\ndisplay_name: AEN\n",
])
def test_reader_2_pad_route_overrides_does_not_fail_open(tmp_path, content):
    """An unusable table must not let the emit path resolve an hw_rev.

    Before this fix `""` and the truncated shape both returned `[]` here
    and `--emit composed-route-table` shipped a wrong-hardware artefact at
    exit 0 for `hw_rev: r99`; the tab-indented and scalar shapes escaped
    as a raw `yaml.ScannerError` / `AttributeError` traceback -- a refusal,
    but not a diagnosable one.
    """
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").write_text(content, encoding="utf-8")

    with pytest.raises(OrchestratorError) as excinfo:
        _hwrev_pad_route_overrides("E1M-AEN801", "r99", meta)
    assert "hw-revisions.yaml" in str(excinfo.value)


def test_reader_2_intact_and_absent_tables_are_unchanged(tmp_path):
    """The two answers that must NOT move: an intact table still refuses
    an unknown rev with the same `SdkRevisionUnknown` it always did, and
    an absent one still returns no overrides."""
    intact = _metadata_copy(tmp_path / "intact")
    with pytest.raises(SdkRevisionUnknown):
        _hwrev_pad_route_overrides("E1M-AEN801", "r99", intact)
    # A real rev on the intact tree resolves, so the emit itself is
    # untouched by the new guard.
    assert isinstance(
        _hwrev_pad_route_overrides("E1M-AEN801", "r1", intact), list)

    absent = _metadata_copy(tmp_path / "absent")
    _family_table(absent, "aen").unlink()
    assert _hwrev_pad_route_overrides("E1M-AEN801", "r99", absent) == []


def test_reader_2_emit_refuses_at_exit_1_not_exit_0(tmp_path):
    """End-to-end through the CLI: the fail-open outcome tan-cli#563
    exists to prevent is a wrong-hardware artefact at exit 0."""
    meta = _metadata_copy(tmp_path)
    _family_table(meta, "aen").write_text("", encoding="utf-8")
    board = _board(tmp_path, _AEN_UNKNOWN_REV)

    proc = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "alp_project.py"),
         "--input", str(board), "--emit", "composed-route-table",
         "--metadata-root", str(meta)],
        capture_output=True, text=True)

    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "hw-revisions.yaml" in proc.stderr
    assert "r99" not in proc.stdout      # nothing emitted for the bad rev
    assert "Traceback" not in proc.stderr

