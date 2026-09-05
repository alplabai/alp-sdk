# SPDX-License-Identifier: Apache-2.0
"""`scripts/validate_metadata.py` must never import `alp_model` (#1943).

`validate_metadata.py` is a PR-blocking gate; `scripts/alp_model/` is a
package a future change is expected to delete outright. This fix moved
`resolve_targets()`/`npu_backend()`/`accel_config()` out of
`alp_model.targets` and into `alp_project_loader.py` precisely so the gate no
longer depends on that package -- the whole point goes quiet the moment
someone re-adds the import, which is why the invariant needs its own
assertion rather than living only in a commit message.

AST-walks the real module rather than grepping for the substring
"alp_model", which would also flag this file's own docstring.
"""
from __future__ import annotations

import ast
from pathlib import Path

_TARGET = Path(__file__).resolve().parents[2] / "scripts" / "validate_metadata.py"


def _imported_top_level_modules(source: str) -> set[str]:
    tree = ast.parse(source, filename=str(_TARGET))
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module:
            names.add(node.module.split(".")[0])
    return names


def test_validate_metadata_does_not_import_alp_model():
    imported = _imported_top_level_modules(_TARGET.read_text(encoding="utf-8"))
    assert "alp_model" not in imported, (
        f"{_TARGET.name} imports `alp_model` -- this gate runs on every PR "
        "and must not depend on a package that is due for deletion; move "
        "the needed logic into alp_project_loader.py instead, the way "
        "resolve_targets()/npu_backend()/accel_config() were (#1943)."
    )
