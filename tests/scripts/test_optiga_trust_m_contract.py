"""Pin what the OPTIGA Trust M driver does and does not claim (#1164).

The driver probes the part, reads the Coprocessor UID and runs raw APDU
sessions through Infineon's host library.  It has no typed signing call
and no PSA driver, so no doc or example may present signing as a
feature, and the metadata / header must say what the driver actually
does.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_manifest_and_header_state_the_partial_scope():
    manifest = _text("metadata/chips/optiga_trust_m.yaml")
    header = _text("include/alp/chips/optiga_trust_m.h")

    assert "driver_status:    partial" in manifest
    assert "No typed key/crypto" in manifest
    assert "Driver scope: [PARTIAL]" in header
    assert "typed wrappers and the PSA driver hook are not written yet" in header
    assert "ALP_ERR_NOSUPPORT" not in header


def test_driver_runs_the_host_library_not_a_stub():
    driver = _text("chips/optiga_trust_m/optiga_trust_m.c")

    assert "optiga_util_read_data(" in driver
    assert "optiga_comms_transceive(" in driver
    assert "ALP_ERR_NOSUPPORT" not in driver


def test_secure_element_examples_do_not_claim_typed_signing():
    paths = [
        "examples/aen/aen-secure-element-sign/src/main.c",
        "examples/aen/aen-secure-element-sign/README.md",
        "examples/aen/aen-secure-element-sign/testcase.yaml",
        "examples/v2n/v2n-secure-element-sign/src/main.c",
        "examples/v2n/v2n-secure-element-sign/README.md",
    ]

    combined = "\n".join(_text(path) for path in paths)
    assert "Nothing here writes to the chip" in combined
    assert "build_calc_sign_apdu" not in combined
    assert "MESSAGE_DIGEST" not in combined
    assert "issue an **ECDSA-P256 sign** APDU" not in combined
    assert "CalcSign reply" not in combined


def test_secure_element_docs_do_not_claim_signing():
    docs = "\n".join(
        _text(path)
        for path in [
            "docs/tutorials/06-secure-element-sign.md",
            "docs/bring-up-aen.md",
            "docs/soms/v2n.md",
            "docs/secure-boot.md",
            "examples/README.md",
            "examples/aen/README.md",
            "examples/v2n/README.md",
            "docs/v1.0-readiness.md",
        ]
    )

    assert "OPTIGA Trust M ECDSA-P256 sign" not in docs
    assert "signing path described below is NOT" in docs
    assert "probe-only" not in docs
