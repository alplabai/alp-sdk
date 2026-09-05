# scripts/alp_model/targets.py
"""Compile-target resolution -- re-exported from `alp_project_loader`.

Issue #1943: the resolution logic used to live here, but
`scripts/validate_metadata.py` (a PR-blocking gate) is a genuine consumer of
`resolve_targets()`, and that gate must not import this package -- a
prerequisite for eventually deleting `scripts/alp_model/` without breaking
it. The logic moved to `scripts/alp_project_loader.py`; this module
re-exports the two names `scripts/alp_model/build.py` still imports
(`resolve_targets`, `TargetSpec`) so that caller keeps working unchanged --
no second copy of the logic. Nothing else in this package or its test suite
imports through here any more."""
from __future__ import annotations

from alp_project_loader import (  # noqa: F401  (compat re-export)
    TargetSpec,
    resolve_targets,
)
