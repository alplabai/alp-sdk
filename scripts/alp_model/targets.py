# scripts/alp_model/targets.py
"""Derive .alpmodel compile targets from a SoM SKU (silicon-determined).

Targets come from the host SoC's npus[] *and* from any on-module discrete
accelerator (e.g. the DEEPX DX-M1 on V2M SoMs). A discrete accelerator is any
*other* SoC JSON whose variants[].alp_module_skus lists this SKU -- so the SoC
JSON's alp_module_skus stays the single source of truth (no hardcoded
backend->silicon map)."""
from __future__ import annotations
import json
from dataclasses import dataclass
from pathlib import Path

import yaml

from alp_project_loader import resolve_soc_path


@dataclass(frozen=True)
class TargetSpec:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # SoC ref e.g. "alif:ensemble:e7" | "deepx:dx:m1" | "*"
    accel_config: str       # vela accel-config e.g. "ethos-u55-256"; "" when N/A


def _npu_backend(npu_type: str, subtype: str) -> str | None:
    if npu_type.startswith("ethos-u"):
        return "ethos_u"
    if "drp" in npu_type or "drp" in subtype:   # renesas DRP-AI ("drp-ai" / "ai-mac+drp")
        return "drpai"
    if npu_type.startswith("dx") or "deepx" in npu_type:
        return "deepx_dxm1"
    return None


def _accel_config(npu: dict, backend: str) -> str:
    """vela `--accelerator-config` string for one `npus[]` entry, e.g.
    `ethos-u55-256`; `""` for a backend with no per-target accel-config.

    The one machine source of this string -- `_soc_targets()` below and
    `scripts/validate_metadata.py`'s model-perf paired-core cross-check
    both import it, rather than each hand-building the same f-string, so
    the two can't drift (issue #1520 review, PR #1884): a missing
    `mac_per_cycle` on an `ethos_u` npus[] entry is a malformed SoC JSON
    and raises `KeyError` here -- it must never be silently masked into a
    truncated string like `ethos-u55-`."""
    return f"{npu['type']}-{npu['mac_per_cycle']}" if backend == "ethos_u" else ""


def _soc_targets(soc: dict, silicon_ref: str) -> list[TargetSpec]:
    """One TargetSpec per mappable NPU in a SoC's npus[] (deduped by the caller)."""
    out: list[TargetSpec] = []
    for npu in soc.get("npus", []):
        npu_type = npu.get("type", "")
        backend = _npu_backend(npu_type, npu.get("subtype", ""))
        if backend is None:
            continue
        accel = _accel_config(npu, backend)
        out.append(TargetSpec(backend=backend, silicon_ref=silicon_ref, accel_config=accel))
    return out


def _discrete_socs(sku: str, host_ref: str, metadata_root: Path) -> list[tuple[str, dict]]:
    """SoCs (other than the host) whose variants[].alp_module_skus list this SKU --
    i.e. on-module discrete accelerators wired to the host (DEEPX DX-M1 on V2M)."""
    found: list[tuple[str, dict]] = []
    for path in sorted((metadata_root / "socs").glob("**/*.json")):
        soc = json.loads(path.read_text(encoding="utf-8"))
        ref = soc.get("ref")
        if not ref or ref == host_ref:
            continue
        skus = {s for v in soc.get("variants", []) for s in v.get("alp_module_skus", [])}
        if sku in skus:
            found.append((ref, soc))
    return found


def resolve_targets(sku: str, *, metadata_root: Path) -> list[TargetSpec]:
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        raise FileNotFoundError(f"no SoM preset for SKU {sku} at {preset_path}")
    preset = yaml.safe_load(preset_path.read_text(encoding="utf-8"))

    silicon = preset["silicon"]                                 # host SoC, e.g. "alif:ensemble:e7"
    # resolve_soc_path() returns None where the old inline 3-tuple unpack
    # raised ValueError, so re-raise it here: callers distinguish a
    # malformed ref (ValueError) from a well-formed ref naming a spec that
    # isn't on disk (FileNotFoundError, below), and collapsing the two
    # would be a behaviour change, not a refactor (#1096).
    soc_path = resolve_soc_path(silicon, metadata_root)
    if soc_path is None:
        raise ValueError(
            f"malformed silicon ref {silicon!r} in {preset_path}: expected "
            f"exactly 3 colon-separated parts (<vendor>:<family>:<part>)"
        )
    if not soc_path.is_file():
        raise FileNotFoundError(f"no SoC spec for {silicon} at {soc_path}")
    host_soc = json.loads(soc_path.read_text(encoding="utf-8"))

    specs: list[TargetSpec] = []
    seen: set[tuple[str, str]] = set()

    def _add(spec: TargetSpec) -> None:
        key = (spec.backend, spec.accel_config)
        if key not in seen:
            seen.add(key)
            specs.append(spec)

    for spec in _soc_targets(host_soc, silicon):                   # host SoC NPUs
        _add(spec)
    for ref, dsoc in _discrete_socs(sku, silicon, metadata_root):  # on-module discrete NPUs
        for spec in _soc_targets(dsoc, ref):
            _add(spec)
    _add(TargetSpec(backend="cpu", silicon_ref="*", accel_config=""))  # CPU always present
    return specs
