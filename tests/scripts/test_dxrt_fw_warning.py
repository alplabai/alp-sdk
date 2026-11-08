"""dxrt-cli warns before -u / -w / -C on Alp modules and refuses nothing."""

from __future__ import annotations

from pathlib import Path

RT = Path(__file__).resolve().parents[2] / "meta-alp-sdk" / "dynamic-layers" / "meta-deepx-m1" / "recipes-runtime"


def test_warning_text_names_the_facts():
    src = (RT / "dx-rt" / "dx-rt" / "alp_fw_warning.cpp").read_text(encoding="utf-8")
    assert "Alp Lab specific DX-M1 firmware" in src
    assert "unusable" in src and "Recovery is not possible in the field" in src
    assert "thermal throttling" in src


def test_no_refusal_path_remains():
    for f in [RT / "dx-rt" / "dx-rt" / "alp_fw_warning.cpp", RT / "dx-rt" / "dx-rt_%.bbappend",
              RT / "dx-driver" / "dx-driver_%.bbappend"]:
        text = f.read_text(encoding="utf-8")
        for needle in ("EPERM", "allowlist", "dxm1-fw-update-allowed", "ALP_DXM1_ALLOW_FW_UPDATE"):
            assert needle not in text, f"{f.name} still mentions {needle}"
    assert not list((RT / "dx-driver").glob("*/*.patch")), "dx-driver must carry no Alp patch"
    assert not list((RT / "dx-rt").glob("*/*.patch")), "dx-rt must carry no source patch"
