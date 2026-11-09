"""E1M-V2N104..106 / E1M-V2M104..106 are the R9A09G056N48GBG twins (#3119):
same PCB and memory tiers as 101..103, N48 silicon (GPU + ISP + security IP).
"""

import json
import re
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
SOM = REPO / "metadata/e1m_modules"
LAYER = REPO / "meta-alp-sdk"
N44_JSON = json.loads((REPO / "metadata/socs/renesas/rzv2n/n44.json").read_text(encoding="utf-8"))

TWINS = {  # N48 SKU -> the N44 SKU it mirrors
    "E1M-V2N104": "E1M-V2N101", "E1M-V2N105": "E1M-V2N102", "E1M-V2N106": "E1M-V2N103",
    "E1M-V2M104": "E1M-V2M101", "E1M-V2M105": "E1M-V2M102", "E1M-V2M106": "E1M-V2M103",
}


def _preset(sku):
    return yaml.safe_load((SOM / f"{sku}.yaml").read_text(encoding="utf-8"))


def _variant(order_code):
    return next(v for v in N44_JSON["variants"] if v["order_code"] == order_code)


def _machine(sku):
    return f"e1m-{sku[4:].lower()}-a55"


def _conf_text(rel):
    """A machine conf/inc plus, recursively, the layer-local confs it `require`s."""
    text = (LAYER / "conf/machine" / rel).read_text(encoding="utf-8")
    for base in re.findall(r"^require conf/machine/((?:include/)?e1m-[\w.-]+)$", text, re.M):
        text += _conf_text(base)
    return text


def test_security_crypto_is_true_only_on_the_n48_skus():
    n48, n44 = _variant("R9A09G056N48GBG"), _variant("R9A09G056N44GBG")
    assert n48["optional_features"]["security_crypto"] is True
    assert n44["optional_features"]["security_crypto"] is False
    assert set(n48["alp_module_skus"]) == set(TWINS)
    assert set(n44["alp_module_skus"]) == set(TWINS.values())
    for sku in TWINS:
        assert _preset(sku)["silicon_variant"] == "R9A09G056N48GBG"
    for sku in TWINS.values():
        assert _preset(sku)["silicon_variant"] == "R9A09G056N44GBG"


def test_twins_mirror_their_n44_sibling():
    for new, old in TWINS.items():
        a, b = _preset(new), _preset(old)
        for key in ("family", "silicon", "pad_routes"):
            assert a[key] == b[key], (new, key)
        assert re.findall(r"(?:dram|flash)_mbit:\s*(\d+)", (SOM / f"{new}.yaml").read_text(encoding="utf-8")) == \
            re.findall(r"(?:dram|flash)_mbit:\s*(\d+)", (SOM / f"{old}.yaml").read_text(encoding="utf-8")), new


def test_n48_override_appears_only_in_n48_machine_conf_chains():
    for sku in TWINS:
        assert "include/e1m-n48.inc" in _conf_text(_machine(sku) + ".conf"), sku
    for sku in TWINS.values():
        assert "e1m-n48" not in _conf_text(_machine(sku) + ".conf"), sku
    inc = (LAYER / "conf/machine/include/e1m-n48.inc").read_text(encoding="utf-8")
    assert 'MACHINEOVERRIDES =. "e1m-n48:"' in inc
    assert not (LAYER / "recipes-kernel/linux/linux-renesas/crypto-arm64-ce.cfg").exists()
