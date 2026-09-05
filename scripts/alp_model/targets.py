# scripts/alp_model/targets.py
"""Compile-target resolution -- re-exported from `alp_project_loader`.

Issue #1943: the resolution logic used to live here, but
`scripts/validate_metadata.py` (a PR-blocking gate) is a genuine consumer of
`resolve_targets()` / the backend + accel-config helpers, and that gate must
not import this package (ADR-0028 Task 6 needs to delete `scripts/alp_model/`
without breaking it). The logic moved to `scripts/alp_project_loader.py`;
this module re-exports it under its historic names so `scripts/alp_model/
build.py` and this package's own test suite (`tests/scripts/
test_alp_model_targets.py`, `test_silicon_ref_single_source.py`) keep
working unchanged -- no second copy of the logic."""
from __future__ import annotations

from alp_project_loader import (  # noqa: F401  (compat re-export)
    TargetSpec,
    accel_config as _accel_config,
    npu_backend as _npu_backend,
    resolve_targets,
)
