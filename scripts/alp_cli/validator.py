"""board.yaml validator with rich diagnostics.

Runs three passes:
  1. schema_pass     - JSON Schema violations (codes ALP-B001..B004).
  2. xref_pass       - cross-references to SoM / preset / pad metadata
                       (codes ALP-B005..B009).
  3. compat_pass     - peripherals vs. SoC capability table
                       (codes ALP-B010+).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jsonschema

from alp_cli.diagnostic import Diagnostic, DiagnosticCollector
from alp_cli.yaml_pos import load_with_positions, node_position

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "board.schema.json"


def _load_schema() -> dict[str, Any]:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def validate_board_yaml(path: Path) -> DiagnosticCollector:
    """Validate a board.yaml file. Returns a DiagnosticCollector."""
    collector = DiagnosticCollector()
    text = path.read_text(encoding="utf-8")
    try:
        data = load_with_positions(text, source=path)
    except Exception as exc:  # YAML parse error
        collector.add(
            Diagnostic(
                severity="error",
                path=path,
                line=1,
                col=1,
                span=1,
                code="ALP-B000",
                message=f"YAML parse error: {exc}",
                hint=None,
            )
        )
        return collector

    schema = _load_schema()
    _schema_pass(data, schema, path, collector)
    # xref + compat passes added in subsequent tasks.
    return collector


def _schema_pass(
    data: dict[str, Any],
    schema: dict[str, Any],
    path: Path,
    collector: DiagnosticCollector,
) -> None:
    # Strip __pos__ keys before handing to jsonschema (they're not in the schema).
    clean = _strip_pos(data)
    validator = jsonschema.Draft7Validator(schema)
    for err in validator.iter_errors(clean):
        diag = _schema_error_to_diagnostic(err, data, path)
        if diag is not None:
            collector.add(diag)


def _strip_pos(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            k: _strip_pos(v)
            for k, v in value.items()
            if not (isinstance(k, str) and k.startswith("__"))
        }
    if isinstance(value, list):
        return [_strip_pos(v) for v in value]
    return value


def _walk(data: dict[str, Any], path_seq: list[Any]) -> dict[str, Any] | None:
    """Walk a path through the position-augmented document."""
    cursor: Any = data
    for step in path_seq:
        if isinstance(cursor, dict) and step in cursor:
            cursor = cursor[step]
        elif isinstance(cursor, list) and isinstance(step, int) and step < len(cursor):
            cursor = cursor[step]
        else:
            return None
    return cursor if isinstance(cursor, dict) else None


def _schema_error_to_diagnostic(
    err: jsonschema.ValidationError, data: dict[str, Any], path: Path
) -> Diagnostic | None:
    abs_path = list(err.absolute_path)
    parent = _walk(data, abs_path[:-1]) if abs_path else data
    line = parent.get("__line__", 1) if parent else 1
    col = parent.get("__column__", 1) if parent else 1
    span = 1

    if err.validator == "required":
        missing = err.message.split("'")[1] if "'" in err.message else "?"
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B001",
            message=f"required key '{missing}' is missing",
            hint=f"add a '{missing}:' entry to this block",
        )

    if err.validator == "additionalProperties":
        if abs_path:
            bad_key = abs_path[-1]
        else:
            # jsonschema reports additionalProperties errors at the parent level;
            # the offending key is embedded in the message text.
            import re as _re
            _m = _re.search(r"'([^']+)'", err.message)
            bad_key = _m.group(1) if _m else "?"
        if parent and "__keys__" in parent and bad_key in parent["__keys__"]:
            line, col = node_position(parent, bad_key, target="key")
            span = len(str(bad_key))
        allowed = list(err.schema.get("properties", {}).keys())
        from difflib import get_close_matches

        suggestion = get_close_matches(str(bad_key), allowed, n=1)
        hint = f"did you mean '{suggestion[0]}'?" if suggestion else None
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B002",
            message=f"unknown key '{bad_key}'",
            hint=hint,
        )

    if err.validator in {"enum", "pattern"}:
        if abs_path and parent and "__keys__" in parent:
            key = abs_path[-1]
            if key in parent["__keys__"]:
                line, col = node_position(parent, key, target="value")
                span = max(1, len(str(parent.get(key, ""))))
        if err.validator == "enum":
            allowed = err.schema.get("enum", [])
            hint = f"expected one of: {', '.join(map(repr, allowed))}"
        else:
            hint = f"value must match pattern: {err.schema.get('pattern')}"
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B003",
            message=err.message,
            hint=hint,
        )

    if err.validator == "type":
        if abs_path and parent and "__keys__" in parent:
            key = abs_path[-1]
            if key in parent["__keys__"]:
                line, col = node_position(parent, key, target="value")
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=1,
            code="ALP-B004",
            message=err.message,
            hint=f"expected type: {err.schema.get('type')}",
        )

    # Fallback for any validator we haven't mapped yet.
    return Diagnostic(
        severity="error",
        path=path,
        line=line,
        col=col,
        span=1,
        code="ALP-B099",
        message=err.message,
        hint=None,
    )
