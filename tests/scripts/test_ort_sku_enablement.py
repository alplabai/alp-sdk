"""Pin which E1M MACHINEs ship the ONNX Runtime CPU floor (#1259)."""

import re
from pathlib import Path

LAYER = Path(__file__).resolve().parents[2] / "meta-alp-sdk"
# Enabling another SKU is a deliberate edit here, next to its bench pass.
ORT_MACHINES = {
    "e1m-v2m101-a55",
    "e1m-v2m102-a55",
    "e1m-v2m103-a55",
    "e1m-v2n101-a55",
    "e1m-v2n102-a55",
    "e1m-v2n103-a55",
    "e1m-v2m104-a55",
    "e1m-v2m105-a55",
    "e1m-v2m106-a55",
    "e1m-v2n104-a55",
    "e1m-v2n105-a55",
    "e1m-v2n106-a55",
}


def _conf_text(name):
    """A machine conf's text plus, recursively, the sibling machine confs it
    `require`s (the N48 twins E1M-V2x104..106 inherit their 101..103 base)."""
    text = (LAYER / "conf" / "machine" / f"{name}.conf").read_text(encoding="utf-8")
    for base in re.findall(r"^require conf/machine/(e1m-[\w-]+)\.conf$", text, re.M):
        text += _conf_text(base)
    return text


def test_ort_enabled_on_exactly_the_pinned_machines():
    enabled = {
        p.stem
        for p in (LAYER / "conf" / "machine").glob("e1m-*.conf")
        if re.search(r'^ALP_ENABLE_ORT_CPU \?= "(1|\$\{@)', _conf_text(p.stem), re.M)
    }
    assert enabled == ORT_MACHINES


def test_alp_sdk_recipe_wires_ort_packageconfig():
    bb = (LAYER / "recipes-core" / "alp-sdk" / "alp-sdk_git.bb").read_text(encoding="utf-8")
    assert "-DALP_SDK_USE_ORT_CPU=ON -DALP_SDK_ORT_REQUIRED=ON" in bb
    assert re.search(r"^PACKAGECONFIG:append = \"\$\{@' ort' if d.getVar\('ALP_ENABLE_ORT_CPU'\) == '1'", bb, re.M)


def test_v2m_keeps_ort_on_under_deepx_and_reuses_dx_rt_ort():
    # V2M stays ORT-on with DEEPX (#1259); only the provider switches to
    # dx-rt's libonnxruntime so one libonnxruntime ships.
    for n in ("v2m101", "v2m102", "v2m103", "v2m104", "v2m105", "v2m106"):
        conf = _conf_text(f"e1m-{n}-a55")
        assert re.search(r'^ALP_ENABLE_ORT_CPU \?= "1"$', conf, re.M)
    bb = (LAYER / "recipes-core" / "alp-sdk" / "alp-sdk_git.bb").read_text(encoding="utf-8")
    assert "'libonnxruntime' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else 'onnxruntime'" in bb
    assert "-DALP_SDK_USE_ORT_CPU=OFF,${ALP_ORT_PKG},${ALP_ORT_PKG}\"" in bb
