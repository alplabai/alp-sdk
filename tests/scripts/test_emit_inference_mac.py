# SPDX-License-Identifier: Apache-2.0
"""
Per-core Ethos-U accelerator sizing in `_emit_inference` (#909 follow-up).

A part can carry two Ethos-U55s of the same variant but different MAC
arrays wired to different cores -- the Alif E3/E5/E7 pair a 256-MAC
high-perf U55 with the M55-HP core and a 128-MAC high-efficiency U55 with
the M55-HE core (`npus[].paired_core` in the SoC JSON).  A 256-MAC command
stream errors a 128-MAC NPU at invoke (register-proven on the E8 U55-HE:
CONFIG reg 0x400e1028=0x00001807, macs_per_cc=7), so the emit MUST size the
accelerator for the NPU the *target slice's core* drives, not the SoC's
most-capable instance.

Run locally:

    python -m pytest tests/scripts/test_emit_inference_mac.py -v
"""

from __future__ import annotations

import json
import sys
import types
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from alp_orchestrate import kconfig as K  # noqa: E402
from alp_orchestrate.models import Slice  # noqa: E402
from alp_orchestrate.paths import METADATA_ROOT as _MR  # noqa: E402


def _emit(sku: str, soc: str, core: str) -> list[str]:
    """Emit the inference Kconfig lines for one SKU/core using real metadata."""
    soc_spec = json.loads((_MR / "socs" / "alif" / "ensemble" / f"{soc}.json").read_text())
    som = yaml.safe_load((_MR / "e1m_modules" / f"{sku}.yaml").read_text())
    proj = types.SimpleNamespace(soc_spec=soc_spec, som_preset=som, sku=sku,
                                 effective_metadata_root=lambda: _MR)
    slice_ = Slice(core_id=core, os="zephyr", inference={"default_arena_kib": 256})
    return K._emit_inference(proj, slice_, som.get("silicon"))


def _ethos_config(lines: list[str]) -> str:
    hits = [ln for ln in lines if ln.startswith("CONFIG_ETHOS_U") and ln.rstrip().endswith("=y")
            and any(ln.startswith(f"CONFIG_ETHOS_U{v}_") for v in ("55", "65", "85"))]
    assert len(hits) == 1, f"expected exactly one ETHOS_U*_<mac> config, got {hits}"
    return hits[0]


# --- U55-only silicon: the emit must pick the core-paired MAC, not max() ----

def test_aen301_he_core_gets_128_mac_u55():
    # M55-HE drives the 128-MAC high-efficiency U55 -- a max() over the E3's
    # two U55s (256 + 128) would wrongly emit 256 and error the NPU at invoke.
    assert _ethos_config(_emit("E1M-AEN301", "e3", "m55_he")) == "CONFIG_ETHOS_U55_128=y"


def test_aen301_hp_core_gets_256_mac_u55():
    assert _ethos_config(_emit("E1M-AEN301", "e3", "m55_hp")) == "CONFIG_ETHOS_U55_256=y"


@pytest.mark.parametrize("sku,soc", [("E1M-AEN501", "e5"), ("E1M-AEN701", "e7")])
def test_other_u55_only_alif_soms_split_by_core(sku, soc):
    assert _ethos_config(_emit(sku, soc, "m55_he")) == "CONFIG_ETHOS_U55_128=y"
    assert _ethos_config(_emit(sku, soc, "m55_hp")) == "CONFIG_ETHOS_U55_256=y"


# --- U85 silicon: most-capable variant, U85 is not core-paired (shared HG) --

@pytest.mark.parametrize("core", ["m55_he", "m55_hp"])
def test_aen801_picks_u85_regardless_of_core(core):
    # The E8 U85 lives on the shared HG subsystem (no paired_core), so both
    # M55 slices resolve to the flagship U85 -- the per-core path falls back
    # to the most-capable variant, unchanged from the pre-#909 behaviour.
    assert _ethos_config(_emit("E1M-AEN801", "e8", core)) == "CONFIG_ETHOS_U85_256=y"


# --- fail-loud when a multi-MAC variant has no core-paired instance ---------

def test_multi_mac_variant_without_paired_core_raises():
    # Synthetic SoC: two U55s of different MAC arrays, NEITHER declaring
    # paired_core -- the emit must refuse to guess (silent max() was the #909
    # bug), not fall back to 256.
    som = yaml.safe_load((_MR / "e1m_modules" / "E1M-AEN301.yaml").read_text())
    soc_spec = {
        "cores": [{"id": "m55_he", "type": "cortex-m55"}, {"id": "m55_hp", "type": "cortex-m55"}],
        "npus": [
            {"type": "ethos-u55", "subtype": "high-perf", "mac_per_cycle": 256, "gops": 204},
            {"type": "ethos-u55", "subtype": "high-efficiency", "mac_per_cycle": 128, "gops": 46},
        ],
    }
    proj = types.SimpleNamespace(soc_spec=soc_spec, som_preset=som, sku="E1M-AEN301",
                                 effective_metadata_root=lambda: _MR)
    slice_ = Slice(core_id="m55_he", os="zephyr", inference={"default_arena_kib": 256})
    with pytest.raises(ValueError, match="paired_core"):
        K._emit_inference(proj, slice_, som.get("silicon"))
