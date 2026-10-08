# SPDX-License-Identifier: Apache-2.0
"""The CM33 -> CA55 doorbell has ONE register map
(include/alp/protocol/v2n_mhu_doorbell.h) and ONE devicetree number
(e1m-v2n-doorbell.dtsi).  Pin the offsets Renesas and the #697 bench fix,
and that the header's GIC SPIs agree with the devicetree/bitbake switch.

Run locally:

    python -m pytest tests/scripts/test_v2n_doorbell_contract.py -q
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "include/alp/protocol/v2n_mhu_doorbell.h"
KERNEL = REPO / "meta-alp-sdk/recipes-kernel/linux"

_BACKSLASH_NL = chr(92) + chr(10)


def _macros(rsp_ch8: bool) -> dict[str, int]:
    """Evaluate the header's #defines for one doorbell selection."""
    text = HEADER.read_text(encoding="utf-8")
    text = re.sub(r"/[*].*?[*]/", "", text, flags=re.S).replace(_BACKSLASH_NL, " ")
    arm = re.search(r"#if defined[(]ALP_V2N_DOORBELL_RSP_CH8.*?\n(.*?)#else\n(.*?)#endif\s*#endif", text, re.S)
    assert arm, "doorbell selection block not found"
    text = text.replace(arm.group(0), arm.group(1 if rsp_ch8 else 2))

    objs: dict[str, str] = {}
    funcs: dict[str, tuple[str, str]] = {}
    for name, param, body in re.findall(r"^#define[ \t]+(\w+)(?:[(](\w+)[)])?[ \t]+(.+)$", text, re.M):
        if param:
            funcs[name] = (param, body)
        else:
            objs[name] = body

    def expand(expr: str) -> str:
        for _ in range(20):
            prev = expr
            for name, (param, body) in funcs.items():
                expr = re.sub(
                    rf"{name}[(]([^()]*)[)]",
                    lambda m, b=body, p=param: "(" + re.sub(rf"\b{p}\b", "(" + m.group(1) + ")", b) + ")",
                    expr,
                )
            for name, body in objs.items():
                expr = re.sub(rf"\b{name}\b", "(" + body + ")", expr)
            if expr == prev:
                break
        return expr

    out = {}
    for name, body in objs.items():
        expr = re.sub(r"(0x[0-9A-Fa-f]+|[0-9]+)u", r"\1", expand(body))
        out[name] = eval(expr, {"__builtins__": {}})  # noqa: S307 - our own header
    return out


def test_default_doorbell_is_swint12_spi404():
    m = _macros(rsp_ch8=False)
    assert m["ALP_V2N_DOORBELL_SET_OFF"] == 0x8C4  # CM33 0x504808C4, #697
    assert m["ALP_V2N_DOORBELL_CLR_OFF"] == 0x8C8  # A55 0x104808C8
    assert m["ALP_V2N_DOORBELL_GIC_SPI"] == 404


def test_rsp_ch8_doorbell_is_renesas_spi385():
    m = _macros(rsp_ch8=True)
    # Renesas platform_info.h: RSP_INT_SET/CLR_REG(8) = 8*0x20 + 0x10 + 0x4/0x8
    assert m["ALP_V2N_DOORBELL_SET_OFF"] == 0x114
    assert m["ALP_V2N_DOORBELL_CLR_OFF"] == 0x118
    assert m["ALP_V2N_DOORBELL_GIC_SPI"] == 385


def test_forward_kick_stays_channel5():
    assert _macros(rsp_ch8=False)["ALP_MHU_NS_CH5_KICK_SLOT"] == 0xA0


def test_devicetree_default_and_switch_agree_with_header():
    dtsi = (KERNEL / "linux-renesas/e1m-v2n-doorbell.dtsi").read_text(encoding="utf-8")
    default = int(re.search(r"^#define ALP_V2N_DOORBELL_SPI (\d+)$", dtsi, re.M).group(1))
    assert default == _macros(rsp_ch8=False)["ALP_V2N_DOORBELL_GIC_SPI"]
    bbappend = (KERNEL / "linux-renesas_%.bbappend").read_text(encoding="utf-8")
    allowed = re.search(r"ALP_V2N_DOORBELL_SPI'\) not in \(([^)]*)\)", bbappend).group(1)
    assert set(re.findall(r"\d+", allowed)) == {"404", "385"}
    som = (KERNEL / "linux-renesas/e1m-v2n-som.dtsi").read_text(encoding="utf-8")
    assert "GIC_SPI ALP_V2N_DOORBELL_SPI IRQ_TYPE_LEVEL_HIGH" in som
