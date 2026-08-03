# SPDX-License-Identifier: Apache-2.0
"""#1127: validate_metadata.py must FAIL a chip/SoM/board manifest that
repeats a mapping key, instead of silently validating whichever value
`yaml.safe_load`/`json.loads` happened to keep last.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_dupkey", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


class _NullValidator:
    def iter_errors(self, doc):
        return []


def test_check_files_rejects_duplicate_key_yaml(tmp_path_factory):
    vm = _load_vm()
    # _check_files() reports paths relative to REPO, so the fixture must
    # live inside the checkout (tmp_path is under /tmp -- ValueError).
    d = tmp_path_factory.mktemp("dupkey", numbered=True)
    p = REPO / "metadata" / "chips" / f".test-dup-{d.name}.yaml"
    try:
        # Two `chip_id:` keys -- plain yaml.safe_load would silently keep "b".
        p.write_text("chip_id: a\nchip_id: b\n")
        failures = vm._check_files(
            "YAML", [p], _NullValidator(),
            lambda path: vm.strict_yaml_load(path.read_text(encoding="utf-8"), source=path),
            "chip_id",
        )
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "duplicate key" in failures[0][1][0].lower()


def test_check_files_rejects_duplicate_key_json(tmp_path_factory):
    vm = _load_vm()
    d = tmp_path_factory.mktemp("dupkey", numbered=True)
    p = REPO / "metadata" / "chips" / f".test-dup-{d.name}.json"
    try:
        p.write_text('{"chip_id": "a", "chip_id": "b"}')
        failures = vm._check_files(
            "JSON", [p], _NullValidator(),
            lambda path: vm.strict_json_loads(path.read_text(encoding="utf-8"), source=path),
            "chip_id",
        )
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "duplicate key" in failures[0][1][0].lower()
