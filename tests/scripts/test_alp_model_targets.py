# tests/scripts/test_alp_model_targets.py
"""som.sku -> SoM preset -> SoC npus[] -> compile targets."""
from pathlib import Path
from alp_model.targets import resolve_targets, TargetSpec

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_resolve_targets_for_aen701_yields_ethos_u_accel_configs_plus_cpu():
    specs = resolve_targets("E1M-AEN701", metadata_root=_META)
    by = {(s.backend, s.accel_config) for s in specs}
    assert ("ethos_u", "ethos-u55-256") in by
    assert ("ethos_u", "ethos-u55-128") in by
    assert ("cpu", "") in by
    eu = next(s for s in specs if s.backend == "ethos_u")
    assert eu.silicon_ref == "alif:ensemble:e7"
    assert next(s for s in specs if s.backend == "cpu").silicon_ref == "*"


def test_resolve_targets_dedupes_identical_accel_configs():
    specs = resolve_targets("E1M-AEN701", metadata_root=_META)
    ethos = [s for s in specs if s.backend == "ethos_u"]
    assert len(ethos) == 2                                     # one per distinct accel_config


def test_resolve_targets_for_v2n101_yields_drpai_plus_cpu():
    # E1M-V2N101 -> renesas:rzv2n:n44 -> DRP-AI NPU + cpu
    specs = resolve_targets("E1M-V2N101", metadata_root=_META)
    backends = {s.backend for s in specs}
    assert "drpai" in backends
    assert "cpu" in backends
    drp = next(s for s in specs if s.backend == "drpai")
    assert drp.silicon_ref == "renesas:rzv2n:n44"
    assert drp.accel_config == ""              # drpai has no vela-style accel-config


def test_resolve_targets_for_v2m101_folds_in_on_module_deepx():
    # E1M-V2M101 -> host renesas:rzv2n:n44 (drpai) + on-module DEEPX DX-M1
    # (deepx:dx:m1, found via variants[].alp_module_skus) + cpu.
    specs = resolve_targets("E1M-V2M101", metadata_root=_META)
    by = {(s.backend, s.silicon_ref) for s in specs}
    assert ("drpai", "renesas:rzv2n:n44") in by
    assert ("deepx_dxm1", "deepx:dx:m1") in by          # discrete accelerator folded in
    assert ("cpu", "*") in by
    deepx = next(s for s in specs if s.backend == "deepx_dxm1")
    assert deepx.accel_config == ""


def test_resolve_targets_v2n101_has_no_discrete_deepx():
    # regression: V2N101 has no on-module DEEPX, must NOT gain a deepx target
    specs = resolve_targets("E1M-V2N101", metadata_root=_META)
    assert all(s.backend != "deepx_dxm1" for s in specs)
