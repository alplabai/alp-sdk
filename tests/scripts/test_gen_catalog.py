"""Unit tests for scripts/gen_catalog.py.

Covers determinism, the --check gate, and schema-sanity anchors against the
committed metadata/catalog.json (11 SoMs each resolving to a SoC, non-empty
examples, real portable-API headers, and a couple of known presence cells).
"""

import io
import json
import subprocess
import sys
from contextlib import redirect_stderr
from pathlib import Path
from unittest.mock import patch

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
        assert isinstance(s["soc_peripherals"], dict) and s["soc_peripherals"]
        assert isinstance(s["capabilities"], dict)
        assert s["topology"], s["sku"]


def test_known_presence_cells():
    soms = {s["sku"]: s for s in _catalog()["soms"]}
    # AEN801 (Alif E8) declares ethernet and an NPU.
    assert soms["E1M-AEN801"]["soc_peripherals"]["ethernet"] is True
    assert soms["E1M-AEN801"]["soc_peripherals"]["npu"] is True
    # The V2N SoC (n44) has PCIe Gen3; the Alif parts do not.
    assert soms["E1M-V2N101"]["soc_peripherals"]["pcie"] is True
    assert soms["E1M-AEN801"]["soc_peripherals"]["pcie"] is False


def test_named_instance_and_ext_mem_spi_presence():
    """#1155: PDM/SD1/WIFI_SDIO/xSPI must have catalog keys, not be silently
    absent.  sd1/wifi_sdio are pad-route-derived (only the V2N/V2M family's
    pin-mux table names them); xspi_ospi is SoC-level (external_memory_interfaces).
    """
    soms = {s["sku"]: s for s in _catalog()["soms"]}
    v2n = soms["E1M-V2N101"]["soc_peripherals"]
    assert v2n["pdm"] is True
    assert v2n["sd1"] is True
    assert v2n["wifi_sdio"] is True
    assert v2n["xspi_ospi"] is True
    # AEN801 (Alif E8) has a HexSPI external memory interface but no
    # Renesas-style SD1/WIFI_SDIO pin-mux route.
    aen = soms["E1M-AEN801"]["soc_peripherals"]
    assert aen["xspi_ospi"] is True
    assert aen["sd1"] is False
    assert aen["wifi_sdio"] is False
    # NX9101 has no pin-mux table at all yet -- named-instance keys default
    # False (absence of routing evidence), not omitted.
    nx = soms["E1M-NX9101"]["soc_peripherals"]
    assert nx["sd1"] is False
    assert nx["wifi_sdio"] is False


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


def _example(path: str) -> dict:
    """Find one example entry by its `examples/<cat>/<name>` path."""
    for entries in _catalog()["examples"].values():
        for e in entries:
            if e["path"] == path:
                return e
    raise AssertionError(f"no catalog entry for {path!r}")


def test_facets_come_from_resolved_topology_not_raw_yaml():
    """Issue #1283: rpmsg-aen's board.yaml only writes `cores:` for
    a32_cluster + m55_hp -- m55_he is left at the SoM topology default. A
    facet read off the raw YAML would report 2 cores / zephyr peer app
    unstated; the resolved topology (`core_os_topology`, the same resolver
    `--emit os-topology` uses) reports all three, with m55_he's app filled
    in as the SDK's own stock-shim -- the trap the issue is built around.
    """
    e = _example("examples/multicore/rpmsg-aen")
    assert e["coreCount"] == 3
    assert e["osSet"] == ["yocto", "zephyr"]
    cores = {c["id"]: c for c in e["cores"]}
    assert cores["a32_cluster"] == {"id": "a32_cluster", "os": "yocto", "app": "./linux"}
    assert cores["m55_hp"] == {"id": "m55_hp", "os": "zephyr", "app": "./m55_hp"}
    # m55_he: not written anywhere in rpmsg-aen/board.yaml at all.
    assert cores["m55_he"] == {"id": "m55_he", "os": "zephyr", "app": "alp-stock-shim"}


def test_facets_single_effective_core():
    e = _example("examples/audio/audio-noise-suppression")
    assert e["coreCount"] == 1
    assert e["osSet"] == ["zephyr"]
    assert [c["id"] for c in e["cores"]] == ["m33_sm"]


def test_declares_read_from_raw_board_yaml():
    # aen-cc3501e-bringup declares a chip + per-core peripherals, no ipc/models.
    e = _example("examples/aen/aen-cc3501e-bringup")
    assert e["declares"] == {
        "peripherals": True, "chips": True, "ipc": False, "models": False,
    }
    # rpmsg-aen declares ipc + peripherals, no chips/models.
    e2 = _example("examples/multicore/rpmsg-aen")
    assert e2["declares"] == {
        "peripherals": True, "chips": False, "ipc": True, "models": False,
    }


def test_facets_omitted_not_guessed_when_topology_unresolvable():
    """rpmsg-imx93's only hw_rev is `status: tbd` -- `load_board_yaml` raises
    rather than resolving a topology. The catalog must omit the
    topology-derived facets for that one entry (absence, not a guess), while
    the YAML-derived `declares` stays present."""
    e = _example("examples/multicore/rpmsg-imx93")
    assert "cores" not in e
    assert "coreCount" not in e
    assert "osSet" not in e
    assert "declares" in e


def test_unexpected_topology_failure_warns_on_stderr():
    """`_resolved_core_facets` must not blanket-swallow every orchestrator
    failure silently. Only `SdkRevisionNotBuildable` -- the SoM hw_rev whose
    `status:` refuses a build -- is an honest, expected absence. Any OTHER
    OrchestratorError (a synthetic one here) still returns None (facets stay
    omitted, never guessed) but must name the board + the failure on stderr,
    so a future regression is visible at regen time instead of getting
    committed as "in sync"."""
    board_yaml = REPO / "examples" / "aen" / "aen-analog-validate" / "board.yaml"
    buf = io.StringIO()
    with patch.object(gc, "load_board_yaml",
                       side_effect=gc.OrchestratorError("synthetic failure")):
        with redirect_stderr(buf):
            result = gc._resolved_core_facets(board_yaml)
    assert result is None
    stderr = buf.getvalue()
    assert "synthetic failure" in stderr
    assert "aen-analog-validate" in stderr


def test_schema_version_bumped_for_facets():
    assert gc.SCHEMA_VERSION == 2


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


def test_expected_not_buildable_case_stays_silent():
    """The other half, and the one that keeps the channel worth reading.

    rpmsg-imx93's SoM hw_rev is `status: tbd`, so its facets are legitimately
    absent on every run. Warning about it each time would be a permanent
    false alarm printed by every regen and every CI `--check`, which trains
    the reader to ignore the exact stderr line the test above exists to make
    visible.

    Pinned separately from the warn case because a single test asserting only
    "the synthetic failure warns" passes identically whether or not the
    expected case is excluded -- it cannot tell the two apart.
    """
    board_yaml = REPO / "examples" / "multicore" / "rpmsg-imx93" / "board.yaml"
    assert board_yaml.is_file(), "rpmsg-imx93 moved -- repoint this test"

    err = io.StringIO()
    with redirect_stderr(err):
        facets = gc._resolved_core_facets(board_yaml)

    assert facets is None, "a non-buildable hw_rev must yield no resolved facets"
    assert err.getvalue() == "", (
        "the documented not-buildable case must be silent, got: " + err.getvalue()
    )
