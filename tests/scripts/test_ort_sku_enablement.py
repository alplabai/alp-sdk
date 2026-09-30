"""Pin which E1M MACHINEs ship the ONNX Runtime CPU floor (#1259)."""

import re
from pathlib import Path

LAYER = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
# Enabling another SKU is a deliberate edit here, next to its bench pass.
ORT_MACHINES = {
    "e1m-v2m101-a55",
    "e1m-v2m102-a55",
    "e1m-v2m103-a55",
    "e1m-v2n102-a55",
}


def test_ort_enabled_on_exactly_the_pinned_machines():
    enabled = {
        p.stem
        for p in (LAYER / "conf" / "machine").glob("*.conf")
        if re.search(r'^ALP_ENABLE_ORT_CPU \?= "1"', p.read_text(encoding="utf-8"), re.M)
    }
    assert enabled == ORT_MACHINES


def test_alp_sdk_recipe_wires_ort_packageconfig():
    bb = (LAYER / "recipes-core" / "alp-sdk" / "alp-sdk_0.6.bb").read_text(encoding="utf-8")
    assert "-DALP_SDK_USE_ORT_CPU=ON -DALP_SDK_ORT_REQUIRED=ON" in bb
    assert "ALP_ENABLE_ORT_CPU" in bb
