from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_optiga_manifest_and_header_are_probe_only():
    manifest = _text("metadata/chips/optiga_trust_m.yaml")
    header = _text("include/alp/chips/optiga_trust_m.h")

    assert "I2C_STATE probe only" in manifest
    assert "OPEN_APPLICATION + product-info + raw APDU send" not in manifest
    assert "Driver scope: [PROBE-ONLY]" in header
    assert "does not send OPEN_APPLICATION" in header
    assert "return ALP_ERR_NOSUPPORT" in header


def test_optiga_driver_validates_then_returns_nosupport():
    driver = _text("chips/optiga_trust_m/optiga_trust_m.c")
    tests = _text("tests/zephyr/chips/src/test_security.c")

    assert "Argument validation for the future product-info and raw-APDU" in driver
    assert "return ALP_ERR_NOSUPPORT;" in driver
    assert "test_optiga_trust_m_probe_only_api_contract" in tests
    assert "ALP_ERR_INVAL" in tests
    assert "ALP_ERR_NOSUPPORT" in tests


def test_secure_element_examples_do_not_claim_current_signing():
    paths = [
        "examples/aen/aen-secure-element-sign/src/main.c",
        "examples/aen/aen-secure-element-sign/README.md",
        "examples/aen/aen-secure-element-sign/testcase.yaml",
        "examples/v2n/v2n-secure-element-sign/src/main.c",
        "examples/v2n/v2n-secure-element-sign/README.md",
        "examples/v2n/v2n-secure-element-sign/testcase.yaml",
    ]

    combined = "\n".join(_text(path) for path in paths)
    assert "probe-only" in combined
    assert "build_calc_sign_apdu" not in combined
    assert "MESSAGE_DIGEST" not in combined
    assert "issue an **ECDSA-P256 sign** APDU" not in combined
    assert "CalcSign reply" not in combined


def test_secure_element_docs_are_probe_only():
    docs = "\n".join(
        _text(path)
        for path in [
            "docs/tutorials/06-secure-element-sign.md",
            "docs/bring-up-aen.md",
            "docs/soms/v2n.md",
            "examples/README.md",
            "examples/aen/README.md",
            "examples/v2n/README.md",
            "docs/v1.0-readiness.md",
        ]
    )

    assert "Secure-element probe contract" in docs
    assert "OPTIGA Trust M probe-only contract" in docs
    assert "OPTIGA Trust M ECDSA-P256 sign" not in docs
    assert "OPTIGA Trust M init + product info + raw-APDU" not in docs
