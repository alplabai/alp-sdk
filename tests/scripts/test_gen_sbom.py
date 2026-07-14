# SPDX-License-Identifier: Apache-2.0
import json, sys
from pathlib import Path
REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import gen_sbom  # noqa: E402

_LOCK = {"lockVersion": 1, "sdk": {"version": "0.9.0", "revision": "abc"},
         "west": {"projects": [{"name": "hal_alif", "revision": "d1", "groups": []}]},
         "libraries": [{"name": "nanopb", "version": "0.4.9.1", "license": "Zlib"}],
         "python": {}, "digests": {"schemas": "sha256:aa"}, "resolution": {}}


def test_cyclonedx_shape():
    b = gen_sbom.build_sbom(_LOCK)
    assert b["bomFormat"] == "CycloneDX"
    assert b["specVersion"] == "1.5"
    names = {c["name"] for c in b["components"]}
    assert {"alp-sdk", "hal_alif", "nanopb"} <= names


def test_deterministic_no_wallclock():
    a = gen_sbom.build_sbom(_LOCK)
    b = gen_sbom.build_sbom(_LOCK)
    assert gen_sbom.digest_json(a) == gen_sbom.digest_json(b)
    assert "timestamp" not in json.dumps(a).lower() or a["metadata"].get("timestamp") is not None
    # serialNumber must be lock-derived (stable), not random
    assert a["serialNumber"] == b["serialNumber"]


def test_licenses_carried():
    b = gen_sbom.build_sbom(_LOCK)
    nanopb = next(c for c in b["components"] if c["name"] == "nanopb")
    assert nanopb["licenses"][0]["license"]["id"] == "Zlib"


def test_check_sbom_gate_passes_on_real_lock():
    import check_sbom  # noqa: E402
    assert check_sbom.main() == 0
