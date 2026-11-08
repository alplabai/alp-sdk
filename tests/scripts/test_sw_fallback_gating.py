# SPDX-License-Identifier: Apache-2.0
"""Guard for #2555: per-class sw_fallback.c must be Kconfig-gated.

Most sw_fallback.c files fake success (loopbacks, no-op stubs, frozen
clocks).  Linking one unconditionally into a silicon build lets an app
"succeed" against hardware that is not there.  Every
src/backends/<class>/sw_fallback.c in zephyr/CMakeLists.txt must
therefore be added with zephyr_library_sources_ifdef(...) (or inside an
if() that names a *_SW_FALLBACK symbol), except the classes below, whose
fallback is the real production implementation.

Run locally:

    python -m pytest tests/scripts/test_sw_fallback_gating.py -q
"""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CMAKE = REPO / "zephyr" / "CMakeLists.txt"

# soc_info: stamps the build-time soc_ref on every build.
# dsp: the portable CMSIS-DSP / radix-2 body IS the production backend
#      (feature-gated on CONFIG_ALP_SDK_DSP, not a stand-in for hardware).
ALLOWLIST = {"soc_info", "dsp"}

_SRC = re.compile(r"src/backends/(\w+)/sw_fallback\.c")
_CALL = re.compile(r"zephyr_library_sources\w*\(")


def ungated(text: str) -> list[str]:
    bad = []
    for m in _SRC.finditer(text):
        cls = m.group(1)
        if cls in ALLOWLIST:
            continue
        call = list(_CALL.finditer(text, 0, m.start()))[-1]
        if not call.group(0).startswith("zephyr_library_sources("):
            # An _ifdef variant: its first argument must be a *_SW_FALLBACK symbol.
            arg = re.match(r"zephyr_library_sources_ifdef\(\s*(\w+)", text[call.start():])
            if not (arg and arg.group(1).endswith("_SW_FALLBACK")):
                bad.append(cls)
            continue
        # Plain call: acceptable only inside if(... _SW_FALLBACK ...).
        lead = text[max(0, call.start() - 200):call.start()]
        if not re.search(r"if\([^)]*_SW_FALLBACK[^)]*\)\s*$", lead):
            bad.append(cls)
    return bad


def test_every_sw_fallback_is_gated():
    bad = ungated(CMAKE.read_text(encoding="utf-8"))
    assert not bad, (
        f"sw_fallback.c added with plain zephyr_library_sources() for {bad}; "
        "use zephyr_library_sources_ifdef(CONFIG_ALP_SDK_<CLASS>_SW_FALLBACK ...) "
        "or add the class to ALLOWLIST with a reason (#2555)"
    )


def test_detector_flags_plain_and_accepts_gated():
    plain = "zephyr_library_sources(\n  ${D}/src/backends/i2c/sw_fallback.c)\n"
    gated = "zephyr_library_sources_ifdef(CONFIG_X_SW_FALLBACK\n  ${D}/src/backends/i2c/sw_fallback.c)\n"
    in_if = "if(A AND CONFIG_T_SW_FALLBACK)\n    zephyr_library_sources(\n  ${D}/src/backends/tmu/sw_fallback.c)\n"
    allowed = "zephyr_library_sources(\n  ${D}/src/backends/dsp/sw_fallback.c)\n"
    wrong_sym = "zephyr_library_sources_ifdef(CONFIG_I2C\n  ${D}/src/backends/i2c/sw_fallback.c)\n"
    assert ungated(wrong_sym) == ["i2c"]
    assert ungated(plain) == ["i2c"]
    assert ungated(gated) == []
    assert ungated(in_if) == []
    assert ungated(allowed) == []
