import json
import os
import sys
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/alp-lock-v1.schema.json"

sys.path.insert(0, str(REPO / "scripts"))

def test_schema_is_draft2020_closed():
    s = json.loads(SCHEMA.read_text())
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["lockVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)


def _fixture_ws(tmp_path):
    """Minimal Alp SDK-shaped workspace."""
    (tmp_path / "scripts" / "alp_cli").mkdir(parents=True)
    (tmp_path / "scripts" / "alp_cli" / "__init__.py").write_text('__version__ = "9.9.9"\n')
    (tmp_path / "scripts").joinpath("requirements.txt").write_text("cbor2\njsonschema==4.21.1\n")
    (tmp_path / "west.yml").write_text(
        "manifest:\n"
        "  group-filter: [-optional]\n"
        "  projects:\n"
        "    - name: hal_alif\n"
        "      revision: abc123\n"
        "      groups: [hal]\n"
        "    - name: cmsis\n"
        "      revision: v5.9.0\n")
    libs = tmp_path / "metadata" / "libraries"; libs.mkdir(parents=True)
    (libs / "aws-iot.yaml").write_text(
        "schema_version: 1\nname: aws-iot\nversion: v3.1.5\nlicense: Apache-2.0\n"
        "integration:\n  zephyr:\n    west:\n      revision: v3.1.5\n")
    sch = tmp_path / "metadata" / "schemas"; sch.mkdir(parents=True)
    (sch / "a.schema.json").write_text('{"x":1}')
    return tmp_path

def test_build_lock_validates_and_is_deterministic(tmp_path):
    import alp_lock
    ws = _fixture_ws(tmp_path)
    lock1 = alp_lock.build_lock(ws, rev_resolver=lambda r: "deadbeef")
    lock2 = alp_lock.build_lock(ws, rev_resolver=lambda r: "deadbeef")
    jsonschema.validate(lock1, json.loads(SCHEMA.read_text()))
    assert lock1 == lock2
    assert lock1["sdk"] == {"version": "9.9.9", "revision": "deadbeef"}
    # sorted by name
    assert [p["name"] for p in lock1["west"]["projects"]] == ["cmsis", "hal_alif"]
    assert lock1["west"]["groupFilter"] == ["-optional"]
    assert lock1["libraries"][0] == {"name": "aws-iot", "version": "v3.1.5",
                                     "license": "Apache-2.0", "revision": "v3.1.5"}
    assert {"name": "cbor2", "version": None, "hash": None} in lock1["python"]["requirements"]
    assert lock1["digests"]["schemas"].startswith("sha256:")

def test_reject_local_guard(tmp_path):
    import alp_lock
    with __import__("pytest").raises(alp_lock.LockError):
        alp_lock._reject_local("/home/user/secret")
    assert alp_lock._reject_local("v3.1.5") == "v3.1.5"
    assert alp_lock._reject_local("https://github.com/a/b.git") == "https://github.com/a/b.git"


def test_verify_lock_detects_drift(tmp_path):
    import alp_lock
    ws = _fixture_ws(tmp_path)
    locked = alp_lock.build_lock(ws, rev_resolver=lambda r: "v1")
    assert alp_lock.verify_lock(locked, ws, rev_resolver=lambda r: "v1") == []
    # mutate a west revision on disk
    (ws / "west.yml").write_text(
        (ws / "west.yml").read_text().replace("abc123", "9999999"))
    drifts = alp_lock.verify_lock(locked, ws, rev_resolver=lambda r: "v1")
    assert any(d.path == "west.projects[hal_alif].revision"
               and d.locked == "abc123" and d.actual == "9999999" for d in drifts)


import subprocess as _sp

def _run_cli(ws, *args):
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join([str(REPO / "scripts")])
    return _sp.run([sys.executable, str(REPO / "scripts/west_commands/alp_lock.py"),
                    "--workspace", str(ws), *args],
                   capture_output=True, text=True, env=env)

def test_cli_writes_then_check_passes(tmp_path):
    ws = _fixture_ws(tmp_path)
    r = _run_cli(ws)
    assert r.returncode == 0, r.stderr
    assert (ws / "alp.lock").is_file()
    r2 = _run_cli(ws, "--check")
    assert r2.returncode == 0, r2.stdout + r2.stderr

def test_cli_check_fails_on_drift(tmp_path):
    ws = _fixture_ws(tmp_path)
    assert _run_cli(ws).returncode == 0
    (ws / "west.yml").write_text((ws / "west.yml").read_text().replace("abc123", "9999999"))
    r = _run_cli(ws, "--check")
    assert r.returncode == 1
    assert "west.projects[hal_alif].revision" in (r.stdout + r.stderr)
