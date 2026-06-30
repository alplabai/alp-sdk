"""Unit tests for scripts/gen_support_matrix.py.

Covers determinism, the --check gate, and a few known-true / known-false
presence cells anchored to the committed metadata.
"""

import subprocess
import sys
from pathlib import Path

import gen_support_matrix as gsm  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_support_matrix.py"
OUT = REPO / "docs" / "peripheral-support-matrix.md"


def _render() -> str:
    socs = gsm.load_socs()
    mods = gsm.load_modules()
    rows = gsm.build_rows(mods, socs)
    return gsm.render(rows)


def _present(text: str, sku: str, column: str) -> bool:
    """Return the presence cell (True=✅) for `sku` x `column` in the table."""
    header_line = next(ln for ln in text.splitlines()
                       if ln.startswith("| SoM SKU "))
    cols = [c.strip() for c in header_line.strip("|").split("|")]
    idx = cols.index(column)
    row = next(ln for ln in text.splitlines()
               if ln.startswith(f"| {sku} "))
    cells = [c.strip() for c in row.strip("|").split("|")]
    return cells[idx] == gsm.PRESENT


def test_render_is_deterministic():
    assert _render() == _render()


def test_committed_file_matches_generator():
    assert OUT.read_text(encoding="utf-8") == _render()


def test_check_mode_passes_on_committed_file():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_check_mode_fails_when_drifted(tmp_path, monkeypatch):
    drifted = tmp_path / "peripheral-support-matrix.md"
    drifted.write_text("stale\n", encoding="utf-8")
    monkeypatch.setattr(gsm, "OUT", drifted)
    # Drive --check via the module entry by faking argv.
    monkeypatch.setattr(sys, "argv", ["gen_support_matrix.py", "--check"])
    assert gsm.main() == 1


def test_known_presence_cells():
    text = _render()
    # AEN801 (Alif E8) declares ethernet: 1.
    assert _present(text, "E1M-AEN801", "Ethernet")
    # AEN401 (Alif E4) has an empty peripherals block -> Ethernet absent.
    assert not _present(text, "E1M-AEN401", "Ethernet")
    # The V2N SoC (n44) has PCIe Gen3; the Alif parts do not.
    assert _present(text, "E1M-V2N101", "PCIe")
    assert not _present(text, "E1M-AEN801", "PCIe")
    # V2N declares no DAC; the Alif E8 declares dac_12bit.
    assert not _present(text, "E1M-V2N101", "DAC")
    assert _present(text, "E1M-AEN801", "DAC")
    # Every SoM resolves to a SoC with at least one NPU, including the
    # otherwise-sparse NXP i.MX 93 module.
    assert _present(text, "E1M-NX9101", "NPU")


def test_every_module_resolves_to_a_soc():
    socs = gsm.load_socs()
    mods = gsm.load_modules()
    assert len(mods) == 11
    rows = gsm.build_rows(mods, socs)  # raises if any ref is unresolved
    assert len(rows) == 11
