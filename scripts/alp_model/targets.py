# scripts/alp_model/targets.py
"""Derive .alpmodel compile targets from a SoM SKU (silicon-determined)."""
from __future__ import annotations
import json
from dataclasses import dataclass
from pathlib import Path

import yaml


@dataclass(frozen=True)
class TargetSpec:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # SoC ref e.g. "alif:ensemble:e7", or "*" for cpu
    accel_config: str       # vela accel-config e.g. "ethos-u55-256"; "" when N/A


def _npu_backend(npu_type: str, subtype: str) -> str | None:
    if npu_type.startswith("ethos-u"):
        return "ethos_u"
    if "drp" in npu_type or "drp" in subtype:   # renesas DRP-AI (type "drp-ai" or subtype "ai-mac+drp")
        return "drpai"
    if npu_type.startswith("dx") or "deepx" in npu_type:
        return "deepx_dxm1"
    return None


def resolve_targets(sku: str, *, metadata_root: Path) -> list[TargetSpec]:
    preset_path = metadata_root / "e1m_modules" / f"{sku}.yaml"
    if not preset_path.is_file():
        raise FileNotFoundError(f"no SoM preset for SKU {sku} at {preset_path}")
    preset = yaml.safe_load(preset_path.read_text(encoding="utf-8"))

    silicon = preset["silicon"]                                 # e.g. "alif:ensemble:e7"
    vendor, family, variant = silicon.split(":")
    soc_path = metadata_root / "socs" / vendor / family / f"{variant}.json"
    if not soc_path.is_file():
        raise FileNotFoundError(f"no SoC spec for {silicon} at {soc_path}")
    soc = json.loads(soc_path.read_text(encoding="utf-8"))

    specs: list[TargetSpec] = []
    seen: set[tuple[str, str]] = set()
    for npu in soc.get("npus", []):
        npu_type = npu.get("type", "")
        backend = _npu_backend(npu_type, npu.get("subtype", ""))
        if backend is None:
            continue
        if backend == "ethos_u":
            accel = f"{npu_type}-{npu['mac_per_cycle']}"        # e.g. ethos-u55-256
        else:
            accel = ""
        key = (backend, accel)
        if key in seen:
            continue
        seen.add(key)
        specs.append(TargetSpec(backend=backend, silicon_ref=silicon, accel_config=accel))

    specs.append(TargetSpec(backend="cpu", silicon_ref="*", accel_config=""))
    return specs
