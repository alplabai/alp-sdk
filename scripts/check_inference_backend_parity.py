#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Inference backend/format <-> dispatcher canonicalisation gate.

Every accelerator backend a SoM preset selects (`inference.preferred_backend`,
per-target `backend`) and every model `blob_format` it pins must be a string
the device-side dispatcher actually decodes -- i.e. a literal in the
`_backend_enum` / `_fmt_enum` switches of:

    src/backends/inference/alp_model_select.c

That C source is the single source of truth for the canonical names; this gate
parses the string literals out of it (no second hand-maintained list) and fails
(exit 1) if any preset declares a name the dispatcher does NOT know.

This is the root-cause gate for the GAP_REVIEW "naming canonicalisation
incomplete" finding -- the DEEPX drift (`DEEPX_DX` / `deepx_dx` / `dx-com`
vs canonical `deepx_dxm1` / `dxnn`).  A non-canonical backend string decodes
silently to the AUTO/TFLITE sentinel on-device (NOT an error), so the
mis-selection would never surface at runtime; here it fails CI immediately.

Run locally:

    python3 scripts/check_inference_backend_parity.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SELECT_C = REPO / "src" / "backends" / "inference" / "alp_model_select.c"
PRESETS = REPO / "metadata" / "e1m_modules"

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("check_inference_backend_parity: PyYAML is required.  "
             "Install via `pip install pyyaml`.")


def _enum_literals(source: str, func: str) -> set[str]:
    """Extract the strcmp(s, "...") literals inside a `func` definition.

    The canonical set is whatever the dispatcher compares against; the
    trailing `return <SENTINEL>` (AUTO / TFLITE) carries no literal and is
    intentionally excluded -- a preset must name a real backend, not rely
    on the silent default.
    """
    # Body = from the function signature to the first closing brace at col 0.
    m = re.search(rf"^static\s+\w[\w ]*\b{re.escape(func)}\s*\([^)]*\)\s*\n\{{(.*?)\n\}}",
                  source, re.DOTALL | re.MULTILINE)
    if not m:
        raise SystemExit(f"check_inference_backend_parity: could not locate "
                         f"{func}() in {SELECT_C.relative_to(REPO)}")
    return set(re.findall(r'strcmp\(s,\s*"([^"]+)"\)', m.group(1)))


def _collect_preset_names(path: Path) -> list[tuple[str, str, str]]:
    """Return (field, backend_or_fmt_kind, value) tuples from a preset."""
    doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    inf = doc.get("inference") or {}
    out: list[tuple[str, str, str]] = []
    pb = inf.get("preferred_backend")
    if isinstance(pb, str):
        out.append(("inference.preferred_backend", "backend", pb))
    for i, t in enumerate(inf.get("targets") or []):
        if isinstance(t, dict):
            if isinstance(t.get("backend"), str):
                out.append((f"inference.targets[{i}].backend", "backend", t["backend"]))
            if isinstance(t.get("blob_format"), str):
                out.append((f"inference.targets[{i}].blob_format", "format", t["blob_format"]))
    return out


def main() -> int:
    source = SELECT_C.read_text(encoding="utf-8")
    canonical = {
        "backend": _enum_literals(source, "_backend_enum"),
        "format": _enum_literals(source, "_fmt_enum"),
    }
    # Documented default sentinels the dispatcher accepts WITHOUT a strcmp
    # case: "auto" -> ALP_INFERENCE_BACKEND_AUTO (let the selector choose),
    # "tflite" -> ALP_INFERENCE_MODEL_TFLITE (the code comments it as the
    # "tflite" + default case).  These are legitimate explicit opt-ins; a
    # garbage string still flags because it is neither a strcmp literal nor
    # one of these documented sentinels.
    canonical["backend"].add("auto")
    canonical["format"].add("tflite")
    print(f"canonical backends: {sorted(canonical['backend'])}")
    print(f"canonical formats:  {sorted(canonical['format'])}")
    print()

    errors: list[str] = []
    checked = 0
    for path in sorted(PRESETS.glob("E1M-*.yaml")):
        rel = path.relative_to(REPO)
        for field, kind, value in _collect_preset_names(path):
            checked += 1
            if value not in canonical[kind]:
                errors.append(
                    f"{rel}: {field} = {value!r} is not a canonical {kind} the "
                    f"dispatcher decodes; expected one of "
                    f"{sorted(canonical[kind])}")
            else:
                print(f"OK   {rel}  ({field}={value})")

    print()
    if errors:
        for e in errors:
            print(f"inference-backend: {e}", file=sys.stderr)
        print(f"\ncheck_inference_backend_parity: {len(errors)} non-canonical "
              f"name(s).  A backend/format string with no matching case in "
              f"{SELECT_C.relative_to(REPO)} decodes to the silent AUTO/TFLITE "
              f"default on-device.  Fix the preset, or add the case to the "
              f"dispatcher if the backend is genuinely new.", file=sys.stderr)
        return 1
    print(f"check_inference_backend_parity: {checked} backend/format name(s) "
          f"across all presets are canonical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
