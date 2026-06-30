"""Unit tests for scripts/gen_error_catalog.py.

Covers determinism, the --check gate, and source-of-truth completeness:
every ALP_ERR_* enum member and every docs/diagnostics/ALP-B*.md page must
land in the committed catalog.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

import gen_error_catalog as gec  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_error_catalog.py"
OUT = REPO / "metadata" / "error-catalog.json"
HEADER = REPO / "include" / "alp" / "peripheral.h"
DIAG_DIR = REPO / "docs" / "diagnostics"


def _codes() -> dict:
    return gec.build_catalog()["codes"]


def test_render_is_deterministic():
    assert gec.render(gec.build_catalog()) == gec.render(gec.build_catalog())


def test_committed_file_matches_generator():
    assert OUT.read_text(encoding="utf-8") == gec.render(gec.build_catalog())


def test_committed_file_is_valid_json_with_expected_shape():
    data = json.loads(OUT.read_text(encoding="utf-8"))
    assert "_banner" in data
    assert "AUTO-GENERATED" in data["_banner"]
    assert isinstance(data["codes"], dict)
    for code, entry in data["codes"].items():
        assert entry["code"] == code
        assert entry["kind"] in ("api-error", "runtime-diagnostic")
        assert entry["doc"]  # always present


def test_check_mode_passes_on_committed_file():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_check_mode_fails_when_drifted(tmp_path, monkeypatch):
    drifted = tmp_path / "error-catalog.json"
    drifted.write_text("stale\n", encoding="utf-8")
    monkeypatch.setattr(gec, "OUT", drifted)
    monkeypatch.setattr(sys, "argv", ["gen_error_catalog.py", "--check"])
    assert gec.main() == 1


def test_every_alp_err_enum_member_is_catalogued():
    """Every ALP_ERR_* in the header's enum body must appear in the catalog."""
    src = HEADER.read_text(encoding="utf-8")
    body = re.search(
        r"typedef\s+enum\s*\{(.*?)\}\s*alp_status_t\s*;", src, re.DOTALL
    ).group(1)
    members = set(re.findall(r"\bALP_ERR_[A-Z0-9_]+\b", body))
    assert members, "no ALP_ERR_* members found -- header parse broke"
    codes = _codes()
    for m in members:
        assert m in codes, f"{m} missing from the catalog"
        assert codes[m]["kind"] == "api-error"
        # The enum carries a Doxygen comment for every error member.
        assert codes[m].get("summary"), f"{m} lost its summary"


def test_every_diagnostic_doc_is_catalogued():
    docs = sorted(DIAG_DIR.glob("ALP-B*.md"))
    assert docs, "no ALP-B*.md diagnostic docs found"
    codes = _codes()
    for path in docs:
        code = path.stem
        assert code in codes, f"{code} missing from the catalog"
        assert codes[code]["kind"] == "runtime-diagnostic"
        assert codes[code]["doc"] == f"docs/diagnostics/{code}.md"


def test_no_text_is_invented_for_api_errors():
    """api-error summaries are lifted verbatim from the enum comments."""
    codes = _codes()
    entry = codes["ALP_ERR_NO_BACKEND"]
    assert "no blob for any backend" in entry["summary"]
    # No cause/fix is sourced for api-errors -> those fields are omitted.
    assert "fix" not in entry


def test_section_parsing_lifts_cause_and_fix(tmp_path, monkeypatch):
    """A richer diagnostic doc with ## Cause / ## Fix sections is parsed."""
    diag = tmp_path / "ALP-B999.md"
    diag.write_text(
        "# ALP-B999: pad route conflict\n\n"
        "Two pads claim the same SoC ball.\n\n"
        "## Cause\n\nThe board.yaml assigns one ball twice.\n\n"
        "## Fix\n\nDrop the duplicate pad entry.\n",
        encoding="utf-8",
    )
    entry = gec.parse_runtime_diagnostic(diag)
    assert entry["code"] == "ALP-B999"
    assert entry["summary"] == "pad route conflict"
    assert entry["cause"] == "The board.yaml assigns one ball twice."
    assert entry["fix"] == "Drop the duplicate pad entry."
