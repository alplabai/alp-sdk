# SPDX-License-Identifier: Apache-2.0
"""alp-mcp: a stdio Model Context Protocol (MCP) server for the Alp SDK.

The server exposes the Alp SDK's single-source surface to an MCP-capable agent
(Claude Desktop / Claude Code / any MCP client).  It has two families of tools:

  * **DATA tools** -- pure reads of ``metadata/catalog.json`` (the generated,
    machine-readable map of every SoM, example, emit mode, portable-API header
    and CI gate).  No SDK runtime is needed; these never shell out.

  * **LIVE tools** -- thin wrappers that shell the SDK's own CLIs
    (``scripts/validate_board_yaml.py`` and ``python -m alp_orchestrate``) and
    return their real output.  They return a clear, structured error when the
    underlying tool or input file is missing -- they never silently succeed.

The MCP runtime (the ``mcp`` / FastMCP package) is an *optional* dependency:
install the SDK with the ``mcp`` extra (``pip install 'alp-sdk-cli[mcp]'``) to
run the live stdio server.  When it is absent, this module still imports
cleanly and every tool function remains directly callable -- so the tool logic
can be unit-tested without a live MCP client.

Entry point: ``alp-mcp`` (see ``[project.scripts]`` in ``pyproject.toml``),
which calls :func:`main`.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Optional

# ---------------------------------------------------------------------------
# Path resolution.
#
# We locate the SDK checkout (the "repo root") by walking up from this file
# until we find ``metadata/catalog.json``.  This keeps the server working both
# from a source checkout and when installed as a console script, and lets a
# customer override the location explicitly with ``ALP_SDK_ROOT``.
# ---------------------------------------------------------------------------

_CATALOG_REL = Path("metadata") / "catalog.json"


def _find_repo_root() -> Path:
    """Return the Alp SDK checkout root.

    Resolution order:
      1. ``$ALP_SDK_ROOT`` if it points at a tree containing the catalog.
      2. The nearest ancestor of this file that contains
         ``metadata/catalog.json``.
      3. A best-effort fallback (``scripts/alp_mcp`` -> repo) so error messages
         still name a sensible path.
    """
    env_root = os.environ.get("ALP_SDK_ROOT")
    if env_root:
        candidate = Path(env_root).expanduser().resolve()
        if (candidate / _CATALOG_REL).is_file():
            return candidate

    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / _CATALOG_REL).is_file():
            return parent

    # Fallback: scripts/alp_mcp/server.py -> repo root is two levels up.
    return here.parents[2]


REPO_ROOT = _find_repo_root()
SCRIPTS_DIR = REPO_ROOT / "scripts"
CATALOG_PATH = REPO_ROOT / _CATALOG_REL
VALIDATOR_SCRIPT = SCRIPTS_DIR / "validate_board_yaml.py"


# ---------------------------------------------------------------------------
# Catalog access.
# ---------------------------------------------------------------------------

def _load_catalog() -> dict[str, Any]:
    """Load and parse ``metadata/catalog.json``.

    Raises :class:`FileNotFoundError` with a clear message if the catalog is
    not present (e.g. the server is pointed at a tree without the SDK
    metadata) -- the DATA tools surface this rather than returning empty data.
    """
    if not CATALOG_PATH.is_file():
        raise FileNotFoundError(
            f"alp-mcp: SDK catalog not found at {CATALOG_PATH}. "
            "Run the server from an alp-sdk checkout, or set ALP_SDK_ROOT to "
            "the SDK root (the directory containing metadata/catalog.json)."
        )
    return json.loads(CATALOG_PATH.read_text(encoding="utf-8"))


def _norm_sku(sku: str) -> str:
    """Normalise a SoM SKU for case-insensitive matching."""
    return sku.strip().upper()


def _find_som(catalog: dict[str, Any], sku: str) -> Optional[dict[str, Any]]:
    want = _norm_sku(sku)
    for som in catalog.get("soms", []):
        if _norm_sku(som.get("sku", "")) == want:
            return som
    return None


# ===========================================================================
# DATA tools -- pure reads of metadata/catalog.json.
# ===========================================================================

def list_soms() -> list[dict[str, Any]]:
    """List every E1M / E1M-X System-on-Module (SoM) SKU the SDK supports.

    Returns one entry per SoM with its SKU, product family, silicon string,
    SoC part and core topology (which cores run which OS/board target). Use
    ``som_info`` for the full per-SoM record including its peripheral map.
    """
    catalog = _load_catalog()
    out: list[dict[str, Any]] = []
    for som in catalog.get("soms", []):
        out.append(
            {
                "sku": som.get("sku"),
                "family": som.get("family"),
                "silicon": som.get("silicon"),
                "soc_part": som.get("soc_part"),
                "cores": sorted((som.get("topology") or {}).keys()),
            }
        )
    return out


def som_info(sku: str) -> dict[str, Any]:
    """Return the full catalog record for one SoM SKU.

    ``sku`` is matched case-insensitively (e.g. ``E1M-AEN801`` or
    ``e1m-aen801``). The record includes the peripheral map, capability flags,
    silicon string and per-core topology. Returns an ``{"error": ...}`` object
    (with the list of known SKUs) if the SKU is unknown.
    """
    catalog = _load_catalog()
    som = _find_som(catalog, sku)
    if som is None:
        known = [s.get("sku") for s in catalog.get("soms", [])]
        return {"error": f"unknown SoM SKU: {sku!r}", "known_skus": known}
    return som


def peripheral_support(
    sku: Optional[str] = None, peripheral: Optional[str] = None
) -> dict[str, Any]:
    """Answer peripheral-support questions from the catalog peripheral map.

    Three modes:
      * ``peripheral`` only -> which SoMs expose that peripheral
        (e.g. ``peripheral="pcie"`` -> the V2N / V2M SKUs).
      * ``sku`` only -> the full peripheral map for that SoM (name -> bool).
      * both -> a single boolean: does ``sku`` expose ``peripheral``?

    Peripheral names match the catalog keys (``adc``, ``can``, ``ethernet``,
    ``i2c``, ``mipi_csi``, ``npu``, ``pcie``, ``spi``, ``uart``, ``usb`` ...)
    and are matched case-insensitively. Returns ``{"error": ...}`` on an
    unknown SKU or peripheral, or if neither argument is given.
    """
    catalog = _load_catalog()

    if sku is None and peripheral is None:
        return {"error": "provide at least one of 'sku' or 'peripheral'"}

    if sku is not None:
        som = _find_som(catalog, sku)
        if som is None:
            known = [s.get("sku") for s in catalog.get("soms", [])]
            return {"error": f"unknown SoM SKU: {sku!r}", "known_skus": known}
        periph_map: dict[str, bool] = som.get("peripherals", {})
        if peripheral is None:
            return {"sku": som.get("sku"), "peripherals": periph_map}
        key = peripheral.strip().lower()
        if key not in periph_map:
            return {
                "error": f"unknown peripheral: {peripheral!r}",
                "known_peripherals": sorted(periph_map.keys()),
            }
        return {"sku": som.get("sku"), "peripheral": key, "supported": bool(periph_map[key])}

    # peripheral only -> which SoMs support it.
    key = peripheral.strip().lower()  # type: ignore[union-attr]
    all_keys: set[str] = set()
    for som in catalog.get("soms", []):
        all_keys.update((som.get("peripherals") or {}).keys())
    if key not in all_keys:
        return {
            "error": f"unknown peripheral: {peripheral!r}",
            "known_peripherals": sorted(all_keys),
        }
    supported = [
        som.get("sku")
        for som in catalog.get("soms", [])
        if (som.get("peripherals") or {}).get(key)
    ]
    return {"peripheral": key, "supported_by": supported}


def list_examples(
    category: Optional[str] = None, peripheral: Optional[str] = None
) -> list[dict[str, Any]]:
    """List the SDK example applications, optionally filtered.

    Examples are grouped in the catalog by category (``peripheral-io``,
    ``ai``, ``audio``, ``camera-vision``, ``connectivity``, ``display``,
    ``multicore``, ``power-timing``, ``v2n``, ``aen``). Each returned entry has
    ``category``, ``name``, ``path``, the primary ``som`` and a ``summary``.

      * ``category`` -- keep only examples in that category (case-insensitive).
      * ``peripheral`` -- keyword filter; keeps examples whose name, path or
        summary mention the term (e.g. ``peripheral="adc"`` or ``"i2c"``).
    """
    catalog = _load_catalog()
    examples_by_cat: dict[str, list[dict[str, Any]]] = catalog.get("examples", {})

    cat_filter = category.strip().lower() if category else None
    periph_filter = peripheral.strip().lower() if peripheral else None

    out: list[dict[str, Any]] = []
    for cat, items in examples_by_cat.items():
        if cat_filter is not None and cat.lower() != cat_filter:
            continue
        for item in items:
            if periph_filter is not None:
                haystack = " ".join(
                    str(item.get(field, ""))
                    for field in ("name", "path", "summary")
                ).lower()
                if periph_filter not in haystack:
                    continue
            entry = {"category": cat}
            entry.update(item)
            out.append(entry)
    return out


def list_emit_modes() -> list[dict[str, Any]]:
    """List the generator artefacts the SDK can ``--emit`` from a board.yaml.

    Each entry is a ``{"mode", "description"}`` pair (e.g. ``system-manifest``,
    ``ipc-contract-h``, ``dts-reservations``, ``build-plan`` ...). Pass a
    ``mode`` to the ``emit`` tool to generate that artefact for a project.
    """
    catalog = _load_catalog()
    return catalog.get("emit_modes", [])


def portable_api(header: Optional[str] = None) -> list[dict[str, Any]]:
    """List the portable ``<alp/*>`` API headers and their ``alp_*`` functions.

    With no argument, returns every public portable-API header with its
    function list. Pass ``header`` to filter to one header -- matched flexibly
    against the path, the basename or the bare stem (e.g. ``"adc"``,
    ``"adc.h"`` or ``"include/alp/adc.h"`` all select the ADC header).
    """
    catalog = _load_catalog()
    api: list[dict[str, Any]] = catalog.get("portable_api", [])
    if header is None:
        return api
    want = header.strip().lower()
    want_stem = Path(want).name
    if want_stem.endswith(".h"):
        want_stem = want_stem[:-2]
    matched: list[dict[str, Any]] = []
    for entry in api:
        hpath = str(entry.get("header", "")).lower()
        stem = Path(hpath).name
        if stem.endswith(".h"):
            stem = stem[:-2]
        if want in hpath or want_stem == stem:
            matched.append(entry)
    return matched


def list_gates() -> list[dict[str, Any]]:
    """List the SDK's CI gate scripts and what each one enforces.

    Each entry is ``{"script", "purpose"}`` -- the repo-relative path of a
    ``scripts/check_*.py`` gate plus a one-line statement of what it fails on.
    Useful for understanding what a contribution must pass before merge.
    """
    catalog = _load_catalog()
    return catalog.get("gates", [])


# ===========================================================================
# LIVE tools -- shell the SDK runtime, return real output + clear errors.
# ===========================================================================

def _looks_like_yaml_content(text: str) -> bool:
    """Heuristic: does this string look like inline YAML rather than a path?"""
    return ("\n" in text) or (":" in text and not text.strip().endswith((".yaml", ".yml")))


def validate_board_yaml(path_or_content: str) -> dict[str, Any]:
    """Validate a project ``board.yaml`` with the SDK's own validator.

    Accepts either a filesystem **path** to an existing ``board.yaml`` or the
    raw YAML **content** as a string (it is written to a temp file and
    validated). Runs ``scripts/validate_board_yaml.py`` and returns the real
    diagnostics:

        {"ok": bool, "returncode": int, "stdout": str, "stderr": str}

    ``ok`` is true only on a clean validation (exit 0). Returns a clear
    ``{"ok": false, "error": ...}`` -- never a crash -- when the validator
    script is missing or the input cannot be resolved.
    """
    if not VALIDATOR_SCRIPT.is_file():
        return {
            "ok": False,
            "error": f"validator script not found at {VALIDATOR_SCRIPT}",
        }

    candidate = Path(path_or_content)
    tmp_path: Optional[Path] = None
    try:
        if candidate.is_file():
            target = candidate
        elif _looks_like_yaml_content(path_or_content):
            fd, name = tempfile.mkstemp(suffix=".yaml", prefix="alp_mcp_board_")
            os.close(fd)
            tmp_path = Path(name)
            tmp_path.write_text(path_or_content, encoding="utf-8")
            target = tmp_path
        else:
            return {
                "ok": False,
                "error": (
                    f"input is neither an existing file nor recognisable YAML "
                    f"content: {path_or_content!r}"
                ),
            }

        proc = _run(
            [sys.executable, str(VALIDATOR_SCRIPT), "--input", str(target)],
            cwd=REPO_ROOT,
        )
        return {
            "ok": proc.returncode == 0,
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
    finally:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)


def emit(board_yaml_path: str, mode: str) -> dict[str, Any]:
    """Emit a generated artefact for a project via ``python -m alp_orchestrate``.

    Runs ``python -m alp_orchestrate --emit <mode> --input <board_yaml_path>``
    and returns the generated artefact:

        {"ok": bool, "returncode": int, "mode": str, "artifact": str,
         "stderr": str}

    ``mode`` must be one of the catalog ``emit_modes`` (see ``list_emit_modes``)
    -- an unknown mode is rejected up front with the valid list. Returns a
    clear ``{"ok": false, "error": ...}`` when the board.yaml is missing,
    rather than crashing.
    """
    try:
        valid_modes = [m.get("mode") for m in _load_catalog().get("emit_modes", [])]
    except FileNotFoundError as exc:
        return {"ok": False, "error": str(exc)}

    if mode not in valid_modes:
        return {
            "ok": False,
            "error": f"unknown emit mode: {mode!r}",
            "valid_modes": valid_modes,
        }

    board = Path(board_yaml_path)
    if not board.is_file():
        return {"ok": False, "error": f"board.yaml not found: {board_yaml_path}"}

    # Run with the SDK's scripts/ dir importable so `-m alp_orchestrate`
    # resolves; pass an absolute --input so cwd does not matter.
    env = dict(os.environ)
    env["PYTHONPATH"] = (
        str(SCRIPTS_DIR) + os.pathsep + env.get("PYTHONPATH", "")
    ).rstrip(os.pathsep)
    proc = _run(
        [
            sys.executable,
            "-m",
            "alp_orchestrate",
            "--emit",
            mode,
            "--input",
            str(board.resolve()),
        ],
        cwd=REPO_ROOT,
        env=env,
    )
    return {
        "ok": proc.returncode == 0,
        "returncode": proc.returncode,
        "mode": mode,
        "artifact": proc.stdout,
        "stderr": proc.stderr,
    }


# A wall-clock cap on the SDK subprocesses the live tools shell out to. Without
# it a hung validator/orchestrator would block the MCP stdio client forever.
_SUBPROCESS_TIMEOUT_S = 120


def _run(
    cmd: list[str],
    cwd: Optional[Path] = None,
    env: Optional[dict[str, str]] = None,
    timeout: int = _SUBPROCESS_TIMEOUT_S,
) -> subprocess.CompletedProcess[str]:
    """Run a subprocess capturing text stdout/stderr (no exception on nonzero).

    Bounded by ``timeout`` seconds; a timeout is surfaced as a synthetic
    nonzero result (returncode 124, the conventional timeout code) carrying a
    clear stderr message, so callers degrade to ``{"ok": false, ...}`` rather
    than the MCP client hanging or seeing an uncaught ``TimeoutExpired``.
    """
    try:
        return subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        return subprocess.CompletedProcess(
            cmd,
            124,
            stdout=exc.stdout or "",
            stderr=f"timed out after {timeout}s: {' '.join(cmd)}",
        )


# ===========================================================================
# MCP wiring.
# ===========================================================================

# The ordered set of tool functions the server exposes. Kept as data so tests
# can assert the registered surface without a live MCP runtime.
TOOL_FUNCTIONS = [
    list_soms,
    som_info,
    peripheral_support,
    list_examples,
    list_emit_modes,
    portable_api,
    list_gates,
    validate_board_yaml,
    emit,
]

TOOL_NAMES = [fn.__name__ for fn in TOOL_FUNCTIONS]

try:  # The MCP runtime is an optional extra; keep import-safe without it.
    from mcp.server.fastmcp import FastMCP

    _HAVE_MCP = True
except Exception:  # pragma: no cover - exercised only when mcp is absent
    FastMCP = None  # type: ignore[assignment,misc]
    _HAVE_MCP = False


def build_server() -> "FastMCP":
    """Construct the FastMCP server with every tool registered.

    Raises a clear :class:`RuntimeError` when the optional ``mcp`` runtime is
    not installed (install it with ``pip install 'alp-sdk-cli[mcp]'``).
    """
    if not _HAVE_MCP:
        raise RuntimeError(
            "alp-mcp: the 'mcp' package is not installed. Install the SDK's "
            "MCP extra:  pip install 'alp-sdk-cli[mcp]'  (or run via "
            "'uvx --from alp-sdk-cli alp-mcp')."
        )
    server = FastMCP("alp-mcp")
    for fn in TOOL_FUNCTIONS:
        # FastMCP derives the tool name, description and JSON schema from the
        # function name, docstring and type hints.
        server.tool()(fn)
    return server


def main() -> None:
    """Console-script entry point: run the stdio MCP server."""
    server = build_server()
    server.run()  # FastMCP defaults to the stdio transport.


if __name__ == "__main__":
    main()
