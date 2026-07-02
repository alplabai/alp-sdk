"""board.yaml validator with rich diagnostics.

Runs three passes:
  1. schema_pass     - JSON Schema violations (codes ALP-B001..B004).
  2. xref_pass       - cross-references to SoM / preset / pad metadata
                       (codes ALP-B005..B009).
  3. compat_pass     - peripherals vs. SoC capability table
                       (codes ALP-B010+).

This module is also the ONE shared board.schema.json implementation:
`load_board_schema()` / `iter_schema_errors()` are consumed by
`scripts/validate_board_yaml.py` (the customer-side pre-flight `west
alp-build` shells) and `scripts/alp_orchestrate/` (the fan-out loader),
so the schema file, draft dialect, and error ordering are decided in
exactly one place.
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

METADATA = REPO / "metadata"
SOM_DIR = METADATA / "e1m_modules"
PRESET_DIR = METADATA / "boards"
SOC_DIR = METADATA / "socs"


def load_board_schema(schema_path: Path | None = None) -> dict[str, Any]:
    """Load board.schema.json (the SDK's copy unless *schema_path* overrides).

    The one place the schema file is resolved + parsed; every validator
    (this module, validate_board_yaml.py, alp_orchestrate) goes through it.
    """
    return json.loads((schema_path or SCHEMA_PATH).read_text(encoding="utf-8"))


def iter_schema_errors(
    data: dict[str, Any], schema_path: Path | None = None
) -> list[jsonschema.ValidationError]:
    """Validate *data* against board.schema.json; errors sorted by path.

    The shared JSON-Schema pass: one schema file, one draft dialect
    (2020-12, matching the schema's own `$schema` declaration), one error
    ordering -- so every consumer reports identical violations.
    """
    validator = jsonschema.Draft202012Validator(load_board_schema(schema_path))
    return sorted(validator.iter_errors(data),
                  key=lambda e: list(e.absolute_path))


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

    _schema_pass(data, path, collector)
    _xref_pass(data, path, collector)
    _compat_pass(data, path, collector)
    return collector


def _xref_pass(
    data: dict[str, Any], path: Path, collector: DiagnosticCollector
) -> None:
    som = data.get("som") or {}
    sku = som.get("sku")
    if isinstance(sku, str):
        if not _sku_resolves(sku):
            line, col = node_position(som, "sku", target="value")
            collector.add(
                Diagnostic(
                    severity="error",
                    path=path,
                    line=line,
                    col=col,
                    span=len(sku),
                    code="ALP-B005",
                    message=f"SoM SKU '{sku}' does not resolve to a known module",
                    hint=_sku_suggestion(sku),
                )
            )

    preset = data.get("preset")
    if isinstance(preset, str):
        if not (PRESET_DIR / f"{preset}.yaml").is_file():
            line, col = node_position(data, "preset", target="value")
            collector.add(
                Diagnostic(
                    severity="error",
                    path=path,
                    line=line,
                    col=col,
                    span=len(preset),
                    code="ALP-B006",
                    message=f"board preset '{preset}' does not exist",
                    hint=_preset_suggestion(preset),
                )
            )


def _sku_resolves(sku: str) -> bool:
    for _candidate in SOM_DIR.rglob(f"{sku}.yaml"):
        return True
    return False


def _all_skus() -> list[str]:
    return sorted(p.stem for p in SOM_DIR.rglob("*.yaml") if p.stem.startswith("E1M-"))


def _all_presets() -> list[str]:
    return sorted(p.stem for p in PRESET_DIR.glob("*.yaml"))


def _sku_suggestion(sku: str) -> str | None:
    from difflib import get_close_matches

    match = get_close_matches(sku, _all_skus(), n=1)
    return f"did you mean '{match[0]}'?" if match else None


def _preset_suggestion(preset: str) -> str | None:
    from difflib import get_close_matches

    match = get_close_matches(preset, _all_presets(), n=1)
    return f"did you mean '{match[0]}'?" if match else None


# ---------------------------------------------------------------------------
# compat_pass helpers (ALP-B010)
# ---------------------------------------------------------------------------


def _compat_pass(
    data: dict[str, Any], path: Path, collector: DiagnosticCollector
) -> None:
    silicon_ref = _silicon_ref_for_sku(data.get("som", {}).get("sku"))
    if silicon_ref is None:
        return
    soc_caps = _load_soc_caps(silicon_ref)
    if soc_caps is None:
        return

    for core_name, core in (data.get("cores") or {}).items():
        if not isinstance(core, dict):
            continue
        peripherals = core.get("peripherals") or []
        if not isinstance(peripherals, list):
            continue
        for idx, periph in enumerate(peripherals):
            # The board.yaml schema defines peripherals as an array of
            # strings (e.g. ["can", "i2c"]).  The schema pass already
            # rejects non-string entries, so skip anything unexpected.
            if not isinstance(periph, str):
                continue
            kind = periph
            if _soc_has_kind(soc_caps, kind):
                continue
            # Best-effort line: the sequence loader only attaches position
            # metadata to dict items; strings carry no __line__.  Fall back
            # to the core block's opening position.
            line = core.get("__line__", 1)
            col = core.get("__column__", 1)
            collector.add(
                Diagnostic(
                    # warning, not error: SoC peripherals JSON ingestion is
                    # incomplete for several parts (e.g. iMX93 with its
                    # `_pending_reason` placeholder), and some peripheral
                    # categories surface board-side rather than directly on
                    # the SoC (emmc / flash / ethernet via I/O controllers).
                    # A false-positive ALP-B010 must not block the build —
                    # surface the discrepancy and let the customer decide.
                    severity="warning",
                    path=path,
                    line=line,
                    col=col,
                    span=len(kind),
                    code="ALP-B010",
                    message=(
                        f"core '{core_name}': peripheral kind '{kind}' is not "
                        f"listed on silicon '{silicon_ref}' (SoC JSON may be "
                        f"incomplete or the peripheral is board-side)"
                    ),
                    hint=(
                        f"verify the SoC truly lacks {kind} before removing "
                        f"this entry; if the SoC has it but the metadata is "
                        f"stale, update metadata/socs/.../*.json"
                    ),
                )
            )


def _silicon_ref_for_sku(sku: str | None) -> str | None:
    if not sku:
        return None
    for som_path in SOM_DIR.rglob(f"{sku}.yaml"):
        import yaml as _yaml

        text = som_path.read_text(encoding="utf-8")
        doc = _yaml.safe_load(text) or {}
        ref = doc.get("silicon")
        if isinstance(ref, str):
            return ref
    return None


def _load_soc_caps(silicon_ref: str) -> dict[str, int] | None:
    parts = silicon_ref.split(":")
    if len(parts) != 3:
        return None
    vendor, family, part = parts
    fp = SOC_DIR / vendor / family / f"{part}.json"
    if not fp.is_file():
        return None
    doc = json.loads(fp.read_text(encoding="utf-8"))
    peripherals = doc.get("peripherals", {}) if isinstance(doc, dict) else {}
    return peripherals if isinstance(peripherals, dict) else {}


# Map user-facing board.yaml peripheral kind names to the SoC JSON key
# prefixes that represent the underlying silicon capability.  Entries
# here are only needed when the user-facing name differs from the SoC
# JSON key (e.g. 'counter' is a Zephyr driver-model class that maps to
# the SoC's timer_* counters; 'sensor' is a driver-model class with no
# silicon peripheral equivalent so it is always considered present).
_PERIPHERAL_KIND_ALIASES: dict[str, tuple[str, ...]] = {
    # Zephyr COUNTER driver class backed by hardware timers.
    "counter": ("timer",),
    # Zephyr PWM driver class backed by hardware timers / PWM units.
    "pwm": ("timer", "pwm"),
    # Zephyr SENSOR driver class: software abstraction over various
    # I2C/SPI sensors; no dedicated silicon peripheral block required.
    # Always allow it -- the I2C/SPI bus is the actual constraint.
    "sensor": (),  # empty tuple = unconditionally present
}


def _soc_has_kind(caps: dict[str, int], kind: str) -> bool:
    """Map a peripheral 'kind' onto SoC capability keys.

    'kind' is the user-facing peripheral category (i2c, spi, can, ...).
    The SoC JSON uses the same names plus _lp suffixes for low-power
    variants; presence in EITHER counts.  Some peripherals also have
    variant suffixes (e.g. can_fd) -- if the base name matches a key
    whose value > 0 that also counts.

    High-level Zephyr driver classes (counter, pwm, sensor) that do not
    map 1-to-1 to SoC JSON keys are resolved via ``_PERIPHERAL_KIND_ALIASES``.
    """
    # Alias table: if the kind has a (possibly empty) alias list, check
    # those SoC key prefixes instead.  An empty alias list means the kind
    # is always considered present (software-only abstraction).
    if kind in _PERIPHERAL_KIND_ALIASES:
        aliases = _PERIPHERAL_KIND_ALIASES[kind]
        if not aliases:
            return True  # unconditionally present (e.g. 'sensor')
        for alias_prefix in aliases:
            direct_keys = (alias_prefix, f"{alias_prefix}_lp")
            if any((caps.get(k, 0) or 0) > 0 for k in direct_keys):
                return True
            for key, count in caps.items():
                if key == alias_prefix or key.startswith(f"{alias_prefix}_"):
                    if (count or 0) > 0:
                        return True
        return False

    # Direct and LP-suffixed match.
    direct_keys = (kind, f"{kind}_lp")
    if any((caps.get(k, 0) or 0) > 0 for k in direct_keys):
        return True
    # Variant-suffixed match: e.g. user writes 'can', SoC JSON has 'can_fd'.
    for key, count in caps.items():
        if key == kind or key.startswith(f"{kind}_"):
            if (count or 0) > 0:
                return True
    return False


def _schema_pass(
    data: dict[str, Any],
    path: Path,
    collector: DiagnosticCollector,
) -> None:
    # Strip __pos__ keys before handing to jsonschema (they're not in the schema).
    clean = _strip_pos(data)
    for err in iter_schema_errors(clean):
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
