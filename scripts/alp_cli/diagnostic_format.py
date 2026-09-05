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

`machine_json_for_board_yaml()` is the LIBRARY entry point: validate a
board.yaml and get the machine document back, with no command wrapper in
the way. Every consumer of this contract -- the schema gate, an LSP --
calls it directly. `alp_cli.validate` used to be one more caller of it,
but that CLI wrapper itself retired once `tan validate` shipped a native
port (ADR 0020 end-state B, alp-sdk#1368); this module and its published
contract did not retire with it.
"""

from __future__ import annotations

import re
from pathlib import Path, PurePosixPath, PureWindowsPath
from typing import Iterable
from urllib.parse import quote

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


#: A Windows drive letter at the string's own start (`C:...`) -- mirrors
#: tan-cli's `_WINDOWS_DRIVE_RE` (alplabai/tan-cli#1111,
#: python/tan/core/uri_reference.py), the reference implementation for
#: this exact defect (#1909).
_WINDOWS_DRIVE_RE = re.compile(r"^[A-Za-z]:")


def _is_windows_spelled(path: str) -> bool:
    """Whether `path` is WINDOWS-spelled, judged from the string itself --
    the host rendering the diagnostic may not match the path's own
    spelling (e.g. a board.yaml path collected on Windows, formatted on
    Linux CI), so this can't use `os.name` or `Path.as_uri()` alone.

    A leading `/` is decided POSIX before any later backslash is even
    consulted: a backslash is a legal character inside a POSIX filename
    (`/tmp/proj/we\\ird.yaml`), so a leading `/` must win over a stray
    backslash elsewhere in the string."""
    if path.startswith("/"):
        return False
    return "\\" in path or bool(_WINDOWS_DRIVE_RE.match(path))


def _path_to_uri_reference(path: str) -> str:
    """`path` rendered as a valid URI reference (RFC 3986), per #1909.

    Both SARIF 2.1.0's `artifactLocation.uri` and LSP's `DocumentUri`
    define this field as a URI reference, not a bare filesystem path.
    An ABSOLUTE path becomes an absolute `file:` URI
    (`file:///C:/w/proj/board.yaml` for a Windows-spelled root,
    `file:///w/proj/board.yaml` for a POSIX one); a RELATIVE path is
    already a legal URI reference (RFC 3986 SS4.2) and is left relative,
    with `\\` swapped to `/` and percent-encoding applied.

    Mirrors tan-cli's `path_to_uri_reference`
    (alplabai/tan-cli#1111, python/tan/core/uri_reference.py) -- ported
    rather than reimplemented from scratch so the two SARIF/LSP exporters
    agree on this field again.
    """
    if _is_windows_spelled(path):
        win = PureWindowsPath(path)
        if win.is_absolute():
            return win.as_uri()
        return quote(path.replace("\\", "/"), safe="/")
    posix = PurePosixPath(path)
    if posix.is_absolute():
        return posix.as_uri()
    return quote(path, safe="/")


def _uri(diag: Diagnostic) -> str:
    return _path_to_uri_reference(str(diag.path))


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


def machine_json_for_board_yaml(
    path: Path,
    *,
    tool_name: str = "alp",
    tool_version: str | None = None,
) -> dict:
    """Validate *path* and return its schemaVersion:1 machine document.

    The library door onto this contract: byte-identical to what
    `--format json` prints, but with no CLI in the call chain -- so a
    consumer (the schema gate, an LSP, a CI job) binds to the exporter
    rather than to a command wrapper that ADR 0020 retires.

    Only diagnostics are reported. The hard cross-field consistency
    check the CLI runs afterwards (`load_board_yaml`) is a SEPARATE
    contract with no diagnostic-v1 representation, and is deliberately
    not run here.

    Imports `validate_board_yaml` locally rather than at module scope: this
    module is the published machine-diagnostics contract (JSON + SARIF
    exporters), and a consumer wanting only `to_machine_json`/`to_sarif`
    (an LSP formatting an already-collected diagnostic list) should not
    have to pull in the validator -- and, with it, `yaml`/`jsonschema`/
    `alp_project_loader` -- just to import this module.
    """
    from alp_cli.validator import validate_board_yaml

    return to_machine_json(
        validate_board_yaml(path), tool_name=tool_name, tool_version=tool_version
    )


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
