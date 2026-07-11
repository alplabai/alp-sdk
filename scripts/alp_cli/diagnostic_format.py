"""Versioned machine diagnostics format (JSON + SARIF) for `alp validate`.

The Rust-style `render()` in `alp_cli.diagnostic` is the HUMAN renderer and
is untouched by this module. This module is the MACHINE-consumable sibling
demanded by #610 SS4: a versioned JSON document (schema:
metadata/schemas/diagnostic-v1.schema.json) an IDE/LSP or CI job can parse
without scraping terminal prose, plus a SARIF 2.1.0 export for tools that
already speak that format (GitHub code scanning, many IDEs).

Range convention (deliberately DIFFERENT between the two exporters, see
each function's docstring):

  * `to_machine_json` -- LSP convention, ZERO-based line/character.
  * `to_sarif`        -- SARIF 2.1.0 spec convention, ONE-based line/column
                          (SARIF regions are 1-based by spec; do not reuse
                          the LSP zero-based numbers here).

`Diagnostic.line` / `.col` are 1-based (Rust-style, matching the human
renderer's `-->  path:line:col`). Both exporters convert from that single
1-based source; neither mutates `Diagnostic` itself.
"""

from __future__ import annotations

from typing import Iterable

from alp_cli import __version__ as _ALP_CLI_VERSION
from alp_cli.diagnostic import Diagnostic, _doc_url

SCHEMA_VERSION = 1
SARIF_VERSION = "2.1.0"
SARIF_SCHEMA_URI = (
    "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/"
    "sarif-schema-2.1.0.json"
)

# severity -> SARIF `level` (SARIF has a native "note", so this is a direct
# 1:1 map -- unlike the LSP mapping documented in the schema, which has no
# equivalent for "note" and folds it to Information).
_SARIF_LEVEL = {"error": "error", "warning": "warning", "note": "note"}


def _uri(diag: Diagnostic) -> str:
    return diag.path.as_posix() if hasattr(diag.path, "as_posix") else str(diag.path)


def _documentation_uri(diag: Diagnostic) -> str:
    return diag.doc_url or _doc_url(diag.code)


def _lsp_range(diag: Diagnostic) -> dict:
    """Zero-based LSP `Range` derived from the 1-based Diagnostic fields.

    start = (line - 1, col - 1); end = start.character + span, same line
    (the human renderer only ever carets a single source line).
    """
    start_line = diag.line - 1
    start_char = diag.col - 1
    end_char = start_char + max(1, diag.span)
    return {
        "start": {"line": start_line, "character": start_char},
        "end": {"line": start_line, "character": end_char},
    }


def _diagnostic_to_json(diag: Diagnostic) -> dict:
    out: dict = {
        "uri": _uri(diag),
        "range": _lsp_range(diag),
        "severity": diag.severity,
        "code": diag.code,
        "message": diag.message,
    }
    if diag.hint:
        out["hint"] = diag.hint
    out["documentationUri"] = _documentation_uri(diag)
    return out


def to_machine_json(
    diags: Iterable[Diagnostic],
    *,
    tool_name: str = "alp",
    tool_version: str | None = None,
) -> dict:
    """Build the schemaVersion:1 machine document (diagnostic-v1.schema.json).

    Ranges are zero-based (LSP convention) -- see module docstring. The
    `schemaVersion` field is the version/capability handshake: a consumer
    that only understands v1 must reject any other value rather than
    best-effort-parsing it.
    """
    return {
        "schemaVersion": SCHEMA_VERSION,
        "tool": {"name": tool_name, "version": tool_version or _ALP_CLI_VERSION},
        "diagnostics": [_diagnostic_to_json(d) for d in diags],
    }


def _sarif_region(diag: Diagnostic) -> dict:
    """One-based SARIF `region` (SARIF spec convention -- NOT the LSP
    zero-based numbers used by `to_machine_json`). `Diagnostic.line`/`.col`
    are already 1-based, so this is a direct passthrough plus span-width."""
    return {
        "startLine": diag.line,
        "startColumn": diag.col,
        "endLine": diag.line,
        "endColumn": diag.col + max(1, diag.span),
    }


def _diagnostic_to_sarif_result(diag: Diagnostic) -> dict:
    return {
        "ruleId": diag.code,
        "level": _SARIF_LEVEL[diag.severity],
        "message": {"text": diag.message},
        "locations": [
            {
                "physicalLocation": {
                    "artifactLocation": {"uri": _uri(diag)},
                    "region": _sarif_region(diag),
                }
            }
        ],
    }


def _sarif_rules(diags: list[Diagnostic]) -> list[dict]:
    seen: dict[str, dict] = {}
    for d in diags:
        if d.code in seen:
            continue
        seen[d.code] = {
            "id": d.code,
            "helpUri": _documentation_uri(d),
        }
    return list(seen.values())


def to_sarif(
    diags: Iterable[Diagnostic],
    *,
    tool_name: str = "alp",
    tool_version: str | None = None,
) -> dict:
    """Build a SARIF 2.1.0 log (runs[].results[]) for *diags*.

    SARIF `region` is ONE-based by spec -- see module docstring; do not
    reuse `to_machine_json`'s zero-based LSP range for this export.
    """
    diag_list = list(diags)
    return {
        "$schema": SARIF_SCHEMA_URI,
        "version": SARIF_VERSION,
        "runs": [
            {
                "tool": {
                    "driver": {
                        "name": tool_name,
                        "informationUri": "https://github.com/alplabai/alp-sdk",
                        "version": tool_version or _ALP_CLI_VERSION,
                        "rules": _sarif_rules(diag_list),
                    }
                },
                "results": [_diagnostic_to_sarif_result(d) for d in diag_list],
            }
        ],
    }
