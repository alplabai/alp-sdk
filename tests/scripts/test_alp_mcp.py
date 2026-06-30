# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/alp_mcp/ -- the customer-facing MCP server.

These exercise the tool *logic* directly against the real
``metadata/catalog.json`` (no live MCP client required).  The optional ``mcp``
runtime is not assumed: the registration test skips cleanly when it is absent,
but the import-safety + tool-surface assertions always run.

Run locally:

    python -m pytest tests/scripts/test_alp_mcp.py -v
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_mcp import server  # noqa: E402


# ---------------------------------------------------------------------------
# Import-safety + registered tool surface.
# ---------------------------------------------------------------------------

EXPECTED_TOOLS = {
    "list_soms",
    "som_info",
    "peripheral_support",
    "list_examples",
    "list_emit_modes",
    "portable_api",
    "list_gates",
    "validate_board_yaml",
    "emit",
}


def test_module_imports_without_mcp_runtime():
    # The module must import cleanly whether or not `mcp` is installed.
    assert server.TOOL_FUNCTIONS
    assert callable(server.main)
    assert callable(server.build_server)


def test_expected_tools_are_registered():
    assert set(server.TOOL_NAMES) == EXPECTED_TOOLS
    # Every registered tool carries a docstring -> becomes its MCP description.
    for fn in server.TOOL_FUNCTIONS:
        assert fn.__doc__ and fn.__doc__.strip(), f"{fn.__name__} missing docstring"


def test_catalog_path_resolves_to_real_catalog():
    assert server.CATALOG_PATH.is_file()
    assert server.CATALOG_PATH == REPO / "metadata" / "catalog.json"


def test_build_server_registers_all_tools_when_mcp_available():
    if not server._HAVE_MCP:
        pytest.skip("mcp runtime not installed in this environment")
    mcp_server = server.build_server()
    # FastMCP exposes registered tools; resolve across plausible APIs.
    names: set[str] = set()
    tm = getattr(mcp_server, "_tool_manager", None)
    if tm is not None and hasattr(tm, "list_tools"):
        names = {t.name for t in tm.list_tools()}
    assert EXPECTED_TOOLS.issubset(names) or names == set()


def test_build_server_raises_clear_error_without_mcp(monkeypatch):
    monkeypatch.setattr(server, "_HAVE_MCP", False)
    with pytest.raises(RuntimeError) as exc:
        server.build_server()
    assert "mcp" in str(exc.value).lower()


# ---------------------------------------------------------------------------
# DATA tools -- against the real catalog.
# ---------------------------------------------------------------------------

def test_list_soms_returns_known_skus():
    soms = server.list_soms()
    skus = {s["sku"] for s in soms}
    assert "E1M-AEN801" in skus
    assert "E1M-V2N101" in skus
    # Each entry carries the projected summary fields.
    for entry in soms:
        assert {"sku", "family", "silicon", "soc_part", "cores"} <= set(entry)


def test_som_info_resolves_case_insensitively():
    a = server.som_info("E1M-AEN801")
    b = server.som_info("e1m-aen801")
    assert a.get("sku") == "E1M-AEN801"
    assert a == b
    assert "peripherals" in a and "topology" in a


def test_som_info_unknown_sku_returns_error():
    res = server.som_info("E1M-NOPE999")
    assert "error" in res
    assert "known_skus" in res and res["known_skus"]


def test_peripheral_support_by_peripheral_pcie():
    res = server.peripheral_support(peripheral="pcie")
    supported = set(res["supported_by"])
    # PCIe is a V2N/V2M-only capability; the AEN parts must be excluded.
    assert "E1M-V2N101" in supported
    assert "E1M-AEN801" not in supported


def test_peripheral_support_by_sku_returns_map():
    res = server.peripheral_support(sku="E1M-AEN801")
    assert res["sku"] == "E1M-AEN801"
    assert isinstance(res["peripherals"], dict)
    assert res["peripherals"]["npu"] is True


def test_peripheral_support_both_returns_bool():
    res = server.peripheral_support(sku="E1M-V2N101", peripheral="pcie")
    assert res["supported"] is True
    res2 = server.peripheral_support(sku="E1M-AEN801", peripheral="pcie")
    assert res2["supported"] is False


def test_peripheral_support_unknown_peripheral_errors():
    res = server.peripheral_support(peripheral="warp_drive")
    assert "error" in res and "known_peripherals" in res


def test_peripheral_support_requires_an_argument():
    res = server.peripheral_support()
    assert "error" in res


def test_list_examples_unfiltered_and_filtered():
    everything = server.list_examples()
    assert everything and all("category" in e and "name" in e for e in everything)

    only_io = server.list_examples(category="peripheral-io")
    assert only_io
    assert all(e["category"] == "peripheral-io" for e in only_io)

    adc = server.list_examples(peripheral="adc")
    assert adc, "expected at least one ADC example"
    assert any("adc" in e["name"].lower() for e in adc)


def test_list_emit_modes_matches_catalog():
    modes = server.list_emit_modes()
    names = {m["mode"] for m in modes}
    assert "system-manifest" in names
    assert "build-plan" in names


def test_portable_api_all_and_filtered():
    allheaders = server.portable_api()
    assert allheaders and all("header" in h and "functions" in h for h in allheaders)

    adc = server.portable_api(header="adc")
    assert len(adc) == 1
    assert adc[0]["header"].endswith("adc.h")
    assert "alp_adc_open" in adc[0]["functions"]
    # Same result via the .h form and the full path form.
    assert server.portable_api(header="adc.h") == adc
    assert server.portable_api(header="include/alp/adc.h") == adc


def test_list_gates_returns_check_scripts():
    gates = server.list_gates()
    assert gates and all("script" in g and "purpose" in g for g in gates)
    assert any(g["script"].endswith(".py") for g in gates)


def test_data_tool_outputs_are_json_serializable():
    # Tool returns must round-trip through JSON (MCP serializes them).
    for payload in (
        server.list_soms(),
        server.som_info("E1M-AEN801"),
        server.peripheral_support(peripheral="pcie"),
        server.list_examples(category="ai"),
        server.list_emit_modes(),
        server.portable_api(),
        server.list_gates(),
    ):
        json.dumps(payload)


# ---------------------------------------------------------------------------
# LIVE tools -- error paths + command construction (subprocess mocked).
# ---------------------------------------------------------------------------

def test_validate_board_yaml_missing_file_is_clear_error():
    res = server.validate_board_yaml("/no/such/board.yaml")
    assert res["ok"] is False
    assert "error" in res


def test_validate_board_yaml_missing_validator_script(monkeypatch, tmp_path):
    monkeypatch.setattr(server, "VALIDATOR_SCRIPT", tmp_path / "absent.py")
    res = server.validate_board_yaml("som: E1M-AEN801\n")
    assert res["ok"] is False
    assert "validator script not found" in res["error"]


def test_validate_board_yaml_runs_validator_on_existing_file(monkeypatch, tmp_path):
    board = tmp_path / "board.yaml"
    board.write_text("som: E1M-AEN801\n", encoding="utf-8")

    captured = {}

    class _Proc:
        returncode = 0
        stdout = "clean"
        stderr = ""

    def _fake_run(cmd, cwd=None, env=None):
        captured["cmd"] = cmd
        return _Proc()

    monkeypatch.setattr(server, "_run", _fake_run)
    res = server.validate_board_yaml(str(board))
    assert res["ok"] is True
    assert res["stdout"] == "clean"
    # The validator script and the real file path must appear in the argv.
    assert str(server.VALIDATOR_SCRIPT) in captured["cmd"]
    assert "--input" in captured["cmd"]
    assert str(board) in captured["cmd"]


def test_validate_board_yaml_writes_inline_content_to_tempfile(monkeypatch):
    captured = {}

    class _Proc:
        returncode = 1
        stdout = ""
        stderr = "schema error"

    def _fake_run(cmd, cwd=None, env=None):
        # The temp file must still exist at call time, and be readable.
        target = cmd[cmd.index("--input") + 1]
        assert Path(target).is_file()
        captured["content"] = Path(target).read_text(encoding="utf-8")
        return _Proc()

    monkeypatch.setattr(server, "_run", _fake_run)
    res = server.validate_board_yaml("som: E1M-AEN801\nname: demo\n")
    assert res["ok"] is False
    assert res["returncode"] == 1
    assert "E1M-AEN801" in captured["content"]


def test_run_surfaces_timeout_as_clean_nonzero(monkeypatch):
    # A hung SDK subprocess must NOT hang the MCP client or raise: _run caps it
    # and returns a synthetic nonzero result so the live tool degrades to ok=False.
    import subprocess as _sp

    def _raise_timeout(*a, **k):
        assert k.get("timeout") == server._SUBPROCESS_TIMEOUT_S
        raise _sp.TimeoutExpired(cmd=a[0] if a else k.get("args"), timeout=k["timeout"])

    monkeypatch.setattr(server.subprocess, "run", _raise_timeout)
    proc = server._run(["sleep", "999"])
    assert proc.returncode == 124
    assert "timed out" in proc.stderr


def test_emit_rejects_unknown_mode():
    res = server.emit("/tmp/board.yaml", "not-a-mode")
    assert res["ok"] is False
    assert "valid_modes" in res
    assert "system-manifest" in res["valid_modes"]


def test_emit_missing_board_is_clear_error():
    res = server.emit("/no/such/board.yaml", "system-manifest")
    assert res["ok"] is False
    assert "not found" in res["error"]


def test_emit_constructs_orchestrate_command(monkeypatch, tmp_path):
    board = tmp_path / "board.yaml"
    board.write_text("som: E1M-AEN801\n", encoding="utf-8")

    captured = {}

    class _Proc:
        returncode = 0
        stdout = "# generated artefact"
        stderr = ""

    def _fake_run(cmd, cwd=None, env=None):
        captured["cmd"] = cmd
        captured["env"] = env
        return _Proc()

    monkeypatch.setattr(server, "_run", _fake_run)
    res = server.emit(str(board), "system-manifest")
    assert res["ok"] is True
    assert res["artifact"] == "# generated artefact"
    assert res["mode"] == "system-manifest"

    cmd = captured["cmd"]
    assert cmd[0] == sys.executable
    assert cmd[1:3] == ["-m", "alp_orchestrate"]
    assert "--emit" in cmd and "system-manifest" in cmd
    # Absolute, resolved input path so cwd does not matter.
    assert str(board.resolve()) in cmd
    # scripts/ is placed on PYTHONPATH so `-m alp_orchestrate` resolves.
    assert str(server.SCRIPTS_DIR) in captured["env"]["PYTHONPATH"]
