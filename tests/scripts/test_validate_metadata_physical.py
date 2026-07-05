import json, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

def test_chip_schema_exists_and_is_draft2020():
    schema = json.loads((REPO / "metadata/schemas/chip-v1.schema.json").read_text())
    assert schema["$schema"].endswith("2020-12/schema")
    assert schema["additionalProperties"] is True
    assert schema["properties"]["schema_version"]["const"] == 1

def test_validate_metadata_passes_on_real_tree():
    # The full validator must stay green with the new chip pass wired in.
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"],
                       cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/chips/" in r.stdout  # chips are now being checked
