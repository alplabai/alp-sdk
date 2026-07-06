"""Unit tests for scripts/gen_catalog.py.

Covers determinism, the --check gate, and schema-sanity anchors against the
committed metadata/catalog.json (11 SoMs each resolving to a SoC, non-empty
examples, real portable-API headers, and a couple of known presence cells).
"""

import json
import subprocess
import sys
from pathlib import Path

import gen_catalog as gc  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_catalog.py"
OUT = REPO / "metadata" / "catalog.json"


def _catalog() -> dict:
    return gc.build_catalog()


def test_render_is_deterministic():
    assert gc.render(_catalog()) == gc.render(_catalog())


def test_committed_file_matches_generator():
    assert OUT.read_text(encoding="utf-8") == gc.render(_catalog())


def test_check_mode_passes_on_committed_file():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stderr


def test_check_mode_fails_when_drifted(tmp_path, monkeypatch):
    drifted = tmp_path / "catalog.json"
    drifted.write_text("stale\n", encoding="utf-8")
    monkeypatch.setattr(gc, "OUT", drifted)
    monkeypatch.setattr(sys, "argv", ["gen_catalog.py", "--check"])
    assert gc.main() == 1


def test_top_level_schema():
    cat = _catalog()
    assert cat["schema_version"] == gc.SCHEMA_VERSION
    assert "AUTO-GENERATED" in cat["_generated"]
    assert set(cat) == {
        "_generated", "schema_version", "soms", "examples",
        "emit_modes", "portable_api", "gates",
    }


def test_eleven_soms_each_resolve_to_a_soc():
    soms = _catalog()["soms"]
    assert len(soms) == 11
    for s in soms:
        # Every SoM must resolve to a SoC with a concrete part number,
        # a family, and a peripheral-presence map.
        assert s["soc_part"], s["sku"]
        assert s["family"], s["sku"]
        assert isinstance(s["peripherals"], dict) and s["peripherals"]
        assert isinstance(s["capabilities"], dict)
        assert s["topology"], s["sku"]


def test_known_presence_cells():
    soms = {s["sku"]: s for s in _catalog()["soms"]}
    # AEN801 (Alif E8) declares ethernet and an NPU.
    assert soms["E1M-AEN801"]["peripherals"]["ethernet"] is True
    assert soms["E1M-AEN801"]["peripherals"]["npu"] is True
    # The V2N SoC (n44) has PCIe Gen3; the Alif parts do not.
    assert soms["E1M-V2N101"]["peripherals"]["pcie"] is True
    assert soms["E1M-AEN801"]["peripherals"]["pcie"] is False


def test_topology_os_is_structural():
    soms = {s["sku"]: s for s in _catalog()["soms"]}
    topo = soms["E1M-AEN801"]["topology"]
    # Cortex-A cluster runs Yocto with a MACHINE; M55 cores run Zephyr boards.
    assert topo["a32_cluster"]["os"] == "yocto"
    assert "machine" in topo["a32_cluster"]
    assert topo["m55_hp"]["os"] == "zephyr"
    assert "board" in topo["m55_hp"]


def test_examples_non_empty_and_grouped():
    examples = _catalog()["examples"]
    assert examples, "no example categories found"
    total = sum(len(v) for v in examples.values())
    assert total > 0
    for category, entries in examples.items():
        for e in entries:
            assert e["path"].startswith(f"examples/{category}/")
            assert (REPO / e["path"] / "board.yaml").is_file()


def test_portable_api_lists_real_headers_and_functions():
    api = _catalog()["portable_api"]
    assert api
    for h in api:
        assert (REPO / h["header"]).is_file()
        assert h["header"].startswith("include/alp/")
        assert isinstance(h["functions"], list)
        for fn in h["functions"]:
            assert fn.startswith("alp_")
    # adc.h is a known public header that declares alp_adc_open().
    adc = next(h for h in api if h["header"].endswith("adc.h"))
    assert "alp_adc_open" in adc["functions"]


def test_emit_modes_match_cli_choices():
    modes = {m["mode"] for m in _catalog()["emit_modes"]}
    # The orchestrator CLI's documented machine contract (ADR-0014).
    assert "system-manifest" in modes
    assert "build-plan" in modes
    for m in _catalog()["emit_modes"]:
        assert m["description"]


def test_gates_enumerate_check_scripts():
    gates = _catalog()["gates"]
    scripts = {g["script"] for g in gates}
    assert "scripts/check_pin_conflicts.py" in scripts
    for g in gates:
        assert g["script"].startswith("scripts/check_")


def test_catalog_is_valid_json_on_disk():
    json.loads(OUT.read_text(encoding="utf-8"))
