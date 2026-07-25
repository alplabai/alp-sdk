# `alp model` machine-readable envelope — Implementation Plan (Plan A, alp-sdk foundation)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `alp model` CLI emit machine-readable JSON (`--format json`) for `build`, and add `list` / `doctor` / `info` subcommands, so a downstream wrapper (`tan model *`) can surface model compilation + toolchain state through its envelope.

**Architecture:** The Python `alp model` CLI emits the *domain payload* only (a plain JSON document on stdout); the `{command, ok, exitCode, project, data, issues}` envelope is added later by `tan` (mirrors `alp validate` → `tan validate`). This plan touches only `scripts/alp_cli/model.py` and the compiler-adapter interface; the compile pipeline (`build_model`, `resolve_targets`, `read_package`) is reused unchanged. `build --format json` reads the manifest back out of the `.alpmodel` it just wrote via the existing `read_package()` — no change to `build_model`.

**Tech Stack:** Python (click CLI, `click.testing.CliRunner`), pytest (`node --test` is the extension repo; this is the SDK's `tests/scripts/` pytest suite). `cbor2` (already a base dep). No new dependency.

## Global Constraints

- **Python ≥ 3.10.** Windows gate runs use `py -3.11 -m pytest …`; Linux/CI uses `python -m pytest …` (both shown per step). Verified 2026-07-24: this box has `py -3.11` (3.11.3, and the default `python` too) + 3.9 — there is **no `py -3.14`**. Use `py -3.11`; avoid `py -3.9` (too old — fails at collection on `dataclass(slots=True)`).
- **Payload field names mirror the manifest** (`scripts/alp_model/manifest.py`): use `backend`, `silicon_ref`, `blob_format`, `arena`, `requires`, `compiler_version` verbatim — no renaming to `backend_id` etc. The cross-repo spec's §4 table says `backend_id`; that is reconciled to `backend` here (spec updated to match).
- **The Python emits the payload, never the full envelope.** No `command`/`ok`/`exitCode`/`project`/`issues` keys — `tan` owns those.
- **Exit codes:** `0` success; `1` user error (missing model artifact / unreadable package). `doctor` and `list` always exit `0` (they are reports). No new exit-code semantics.
- **No Claude/AI attribution in commits.** Attribute to alpCaner. Work on a `feat/alp-model-envelope` branch off `dev` (see starting-work-on-a-branch); this plan doc lives on the `docs/model-edge-ai-management-design` branch with the spec.
- **Do not modify `build_model` / `resolve_targets` / `read_package` / the adapters' `compile()`** — they are the proven pipeline. The only interface addition is a read-only `probe()` on the adapters (Task 3).

---

### Task 1: `alp model build --format json`

Add a `--format {human,json}` option to `build`. `human` keeps the current `built <path>` echo. `json` emits, per model, the compiled targets + skipped coverage read back from the written `.alpmodel`.

**Files:**
- Modify: `scripts/alp_cli/model.py:31-51` (the `build_cmd`)
- Test: `tests/scripts/test_alp_cli_model.py` (append)

**Interfaces:**
- Consumes: `alp_model.build.build_model(...) -> Path` (unchanged); `alp_model.package.read_package(raw: bytes) -> tuple[Manifest, list[bytes]]`; `Manifest.targets: list[Target]`, `Manifest.coverage: list[Coverage]`.
- Produces (the `build` payload `tan` will wrap):
  ```json
  {"models": [
    {"name": "demo", "source": "/abs/models/m.tflite", "alpmodel_path": "/abs/out/demo.alpmodel",
     "total_bytes": 1234,
     "targets": [{"backend": "cpu", "silicon_ref": "*", "blob_format": "tflite",
                  "accel_config": "", "arena": 0, "blob_bytes": 800,
                  "requires": {"sram_kib": 0, "op_features": []}, "compiler_version": "passthrough"}],
     "skipped": [{"backend": "ethos_u", "accel_config": "ethos-u55-256",
                  "status": "skipped", "reason": "ethos_u compiler not installed"}]}
  ]}
  ```
  On a per-model build failure (no blob compiled), that entry is `{"name","source","error": "<message>","targets": [],"skipped": []}` and the command exits `1`.

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_alp_cli_model.py`:

```python
import json as _json


def test_alp_model_build_json_emits_targets_and_coverage(tmp_path):
    (tmp_path / "models").mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    result = CliRunner().invoke(cli, [
        "model", "build",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(tmp_path / "out"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    payload = _json.loads(result.output)
    model = payload["models"][0]
    assert model["name"] == "demo"
    assert model["alpmodel_path"].endswith("demo.alpmodel")
    assert model["total_bytes"] > 0
    cpu = [t for t in model["targets"] if t["backend"] == "cpu"]
    assert len(cpu) == 1 and cpu[0]["blob_bytes"] > 0
    # ethos_u is a declared AEN801 target; without vela on PATH it is a skip.
    assert all(s["status"] in ("skipped", "incompatible") for s in model["skipped"])
```

- [ ] **Step 2: Run the test to verify it fails**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_build_json_emits_targets_and_coverage -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_build_json_emits_targets_and_coverage -v`
Expected: FAIL — `build` has no `--format` option (`no such option: --format`).

- [ ] **Step 3: Implement the JSON path in `build_cmd`**

In `scripts/alp_cli/model.py`, add imports and a payload builder, and extend `build_cmd`:

```python
import json

from alp_model.package import read_package
```

```python
def _target_payload(mft, blobs) -> tuple[list[dict], list[dict]]:
    targets = [{
        "backend": t.backend, "silicon_ref": t.silicon_ref,
        "blob_format": t.blob_format, "accel_config": t.accel_config,
        "arena": t.arena, "blob_bytes": len(blobs[t.blob]),
        "requires": t.requires, "compiler_version": t.compiler_version,
    } for t in mft.targets]
    skipped = [{
        "backend": c.backend, "accel_config": c.accel_config,
        "status": c.status, "reason": c.reason,
    } for c in mft.coverage]
    return targets, skipped
```

Replace the `for m in models:` loop body in `build_cmd` (add a `output_format` param + a `--format` option matching `validate`'s choice style):

```python
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True,
              help="human: 'built <path>' lines. json: a {models:[...]} payload "
                   "(targets + skipped coverage) for machine consumption.")
def build_cmd(board_path: Path, out_dir: Path, metadata_root: Path,
              output_format: str) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    sku = board["som"]["sku"]
    models = board.get("models", [])
    base = board_path.parent
    if output_format == "human":
        if not models:
            click.echo("no models: declared in board.yaml; nothing to build.")
            return
        for m in models:
            source = (base / m["source"]).resolve()
            out = build_model(sku=sku, name=m["name"], source=source,
                              out_dir=out_dir, metadata_root=metadata_root,
                              compile_opts=_resolve_compile(m.get("compile"), base))
            click.echo(f"built {out}")
        return

    entries: list[dict] = []
    failed = False
    for m in models:
        source = (base / m["source"]).resolve()
        try:
            out = build_model(sku=sku, name=m["name"], source=source,
                              out_dir=out_dir, metadata_root=metadata_root,
                              compile_opts=_resolve_compile(m.get("compile"), base))
        except ValueError as exc:      # build_model raises when no blob compiles
            failed = True
            entries.append({"name": m["name"], "source": str(source),
                            "error": str(exc), "targets": [], "skipped": []})
            continue
        mft, blobs = read_package(out.read_bytes())
        targets, skipped = _target_payload(mft, blobs)
        entries.append({"name": m["name"], "source": str(source),
                        "alpmodel_path": str(out), "total_bytes": out.stat().st_size,
                        "targets": targets, "skipped": skipped})
    click.echo(json.dumps({"models": entries}, indent=2))
    if failed:
        raise SystemExit(1)
```

- [ ] **Step 4: Run the test to verify it passes**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_build_json_emits_targets_and_coverage -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_build_json_emits_targets_and_coverage -v`
Expected: PASS. Also re-run the whole file to confirm no regression:
`py -3.11 -m pytest tests/scripts/test_alp_cli_model.py -v` (all existing + new pass).

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): --format json payload for 'alp model build'"
```

---

### Task 2: `alp model list`

Enumerate `board.yaml` `models[]` with each one's declared compile block and built-artifact status (exists / size / stale).

**Files:**
- Modify: `scripts/alp_cli/model.py` (new `list_cmd` on `model_group`)
- Test: `tests/scripts/test_alp_cli_model.py` (append)

**Interfaces:**
- Consumes: `board.yaml` `models[]` (`name`, `source`, optional `compile`); the build output dir (default `build/models`).
- Produces (the `list` payload):
  ```json
  {"models": [
    {"name": "demo", "source": "models/m.tflite", "compile": null,
     "artifact": {"exists": true, "path": "/abs/build/models/demo.alpmodel",
                  "bytes": 1234, "stale": false}}
  ]}
  ```
  `stale` is `true` when the source file's mtime is newer than the `.alpmodel` (or the artifact is missing → `exists:false`, `stale:false`).

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_alp_cli_model.py`:

```python
def test_alp_model_list_reports_artifact_status(tmp_path):
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.tflite").write_bytes(b"TFL3xxxx")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    out = tmp_path / "build" / "models"
    out.mkdir(parents=True)
    (out / "demo.alpmodel").write_bytes(b"ALPM....")   # newer than source
    result = CliRunner().invoke(cli, [
        "model", "list",
        "--board", str(tmp_path / "board.yaml"),
        "--out", str(out),
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    m = _json.loads(result.output)["models"][0]
    assert m["name"] == "demo"
    assert m["artifact"]["exists"] is True
    assert m["artifact"]["stale"] is False
```

- [ ] **Step 2: Run the test to verify it fails**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_list_reports_artifact_status -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_list_reports_artifact_status -v`
Expected: FAIL — `No such command 'list'`.

- [ ] **Step 3: Implement `list_cmd`**

Add to `scripts/alp_cli/model.py`:

```python
@model_group.command(name="list", help="List board.yaml models: + built .alpmodel status.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=Path("board.yaml"), show_default=True, help="Path to board.yaml.")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Build output directory.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def list_cmd(board_path: Path, out_dir: Path, output_format: str) -> None:
    board = yaml.safe_load(board_path.read_text(encoding="utf-8"))
    base = board_path.parent
    entries: list[dict] = []
    for m in board.get("models", []):
        source = (base / m["source"]).resolve()
        artifact = out_dir / f"{m['name']}.alpmodel"
        exists = artifact.is_file()
        stale = bool(exists and source.is_file()
                     and source.stat().st_mtime > artifact.stat().st_mtime)
        entries.append({
            "name": m["name"], "source": m["source"], "compile": m.get("compile"),
            "artifact": {"exists": exists, "path": str(artifact.resolve()),
                         "bytes": artifact.stat().st_size if exists else 0,
                         "stale": stale},
        })
    if output_format == "json":
        click.echo(json.dumps({"models": entries}, indent=2))
    else:
        for e in entries:
            a = e["artifact"]
            state = "missing" if not a["exists"] else ("stale" if a["stale"] else "built")
            click.echo(f"{e['name']:20} {state:8} {e['source']}")
```

- [ ] **Step 4: Run the test to verify it passes**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_list_reports_artifact_status -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_list_reports_artifact_status -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): 'alp model list' with artifact status"
```

---

### Task 3: `alp model doctor` + adapter `probe()`

Report which NPU compiler toolchains are installed, their version, and (when absent) why. Add a read-only `probe()` to the compiler-adapter interface; do not touch `compile()`.

**Files:**
- Modify: `scripts/alp_model/adapters/__init__.py` (add `tool`, `version()`, `reason()`, `probe()`)
- Modify: `scripts/alp_model/adapters/cpu.py`, `ethos_u.py`, `deepx.py`, `drpai.py` (override `tool` / `version` / `reason`)
- Modify: `scripts/alp_cli/model.py` (new `doctor_cmd`)
- Test: `tests/scripts/test_alp_model_adapters.py` (append) + `tests/scripts/test_alp_cli_model.py` (append)

**Interfaces:**
- Produces (`CompilerAdapter.probe()`): `{"backend": str, "tool": str, "available": bool, "version": str | None, "reason": str | None}`.
- Produces (the `doctor` payload):
  ```json
  {"toolchains": [
    {"backend": "cpu", "tool": "", "available": true, "version": "builtin", "reason": null},
    {"backend": "ethos_u", "tool": "vela", "available": false, "version": null,
     "reason": "vela not on PATH (install the ethos-u-vela / model-compile extra)"}
  ]}
  ```

- [ ] **Step 1: Write the failing test (adapter probe)**

Append to `tests/scripts/test_alp_model_adapters.py`:

```python
def test_probe_reports_unavailable_reason(monkeypatch):
    import shutil as _shutil
    from alp_model.adapters.ethos_u import VelaAdapter
    monkeypatch.setattr(_shutil, "which", lambda _n: None)   # vela absent
    p = VelaAdapter().probe()
    assert p["backend"] == "ethos_u"
    assert p["tool"] == "vela"
    assert p["available"] is False
    assert p["version"] is None
    assert "vela" in p["reason"]


def test_probe_reports_available_version(monkeypatch):
    import shutil as _shutil
    from alp_model.adapters.ethos_u import VelaAdapter
    monkeypatch.setattr(_shutil, "which", lambda n: "/usr/bin/vela" if n == "vela" else None)
    p = VelaAdapter().probe()
    assert p["available"] is True
    assert p["version"].startswith("vela")
    assert p["reason"] is None
```

- [ ] **Step 2: Run to verify it fails**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_model_adapters.py::test_probe_reports_unavailable_reason tests/scripts/test_alp_model_adapters.py::test_probe_reports_available_version -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_model_adapters.py::test_probe_reports_unavailable_reason tests/scripts/test_alp_model_adapters.py::test_probe_reports_available_version -v`
Expected: FAIL — `AttributeError: 'VelaAdapter' object has no attribute 'probe'`.

- [ ] **Step 3: Add `probe()` to the interface + overrides**

In `scripts/alp_model/adapters/__init__.py`, add to `CompilerAdapter` (after `requires_compile_opts`):

```python
    tool: str = ""              # console command probed for availability ("" = builtin, no tool)

    def version(self) -> str:
        """Best-effort tool version string; only meaningful when available."""
        return ""

    def reason(self) -> str:
        """Why this backend is unavailable (empty when available)."""
        return f"{self.tool or self.backend} not available"

    def probe(self) -> dict:
        """Read-only availability report for `alp model doctor`."""
        avail = self.is_available()
        return {"backend": self.backend, "tool": self.tool, "available": avail,
                "version": self.version() if avail else None,
                "reason": None if avail else self.reason()}
```

In `scripts/alp_model/adapters/cpu.py` (CpuAdapter), add:

```python
    tool = ""

    def version(self) -> str:
        return "builtin"
```

In `scripts/alp_model/adapters/ethos_u.py` (VelaAdapter), add:

```python
    tool = "vela"

    def version(self) -> str:
        return _vela_version()

    def reason(self) -> str:
        return "vela not on PATH (install the ethos-u-vela / model-compile extra)"
```

In `scripts/alp_model/adapters/deepx.py` (DeepxAdapter), add:

```python
    tool = "dxcom"

    def version(self) -> str:
        return _dxcom_version()

    def reason(self) -> str:
        return "dxcom not found (dx-com wheel not installed and ALP_DEEPX_SDK_HOME unset)"
```

In `scripts/alp_model/adapters/drpai.py` (DrpaiAdapter), add:

```python
    tool = "drpai-tvm"

    def version(self) -> str:
        home = _tvm_home()
        return _compiler_version(home) if home else "drp-ai_tvm"

    def reason(self) -> str:
        return "ALP_DRPAI_TVM_HOME not set or not a built DRP-AI TVM install"
```

- [ ] **Step 4: Run the probe tests to verify they pass**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_model_adapters.py -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_model_adapters.py -v`
Expected: PASS (new + existing adapter tests).

- [ ] **Step 5: Write the failing test (doctor CLI)**

Append to `tests/scripts/test_alp_cli_model.py`:

```python
def test_alp_model_doctor_lists_all_backends():
    result = CliRunner().invoke(cli, ["model", "doctor", "--format", "json"],
                                catch_exceptions=False)
    assert result.exit_code == 0, result.output
    backends = {t["backend"] for t in _json.loads(result.output)["toolchains"]}
    assert {"cpu", "ethos_u", "drpai", "deepx_dxm1"} <= backends
    cpu = next(t for t in _json.loads(result.output)["toolchains"] if t["backend"] == "cpu")
    assert cpu["available"] is True
```

- [ ] **Step 6: Run to verify it fails**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_doctor_lists_all_backends -v`
Expected: FAIL — `No such command 'doctor'`.

- [ ] **Step 7: Implement `doctor_cmd`**

Add to `scripts/alp_cli/model.py` (import the registry at top: `from alp_model.build import _ADAPTERS`):

```python
@model_group.command(name="doctor", help="Report installed NPU compiler toolchains.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def doctor_cmd(output_format: str) -> None:
    tools = [a.probe() for a in _ADAPTERS]
    if output_format == "json":
        click.echo(json.dumps({"toolchains": tools}, indent=2))
    else:
        for t in tools:
            mark = "ok" if t["available"] else "--"
            ver = t["version"] or t["reason"] or ""
            click.echo(f"[{mark}] {t['backend']:12} {t['tool']:12} {ver}")
```

Note: `_ADAPTERS` is the module-level registry already defined in `build.py`; importing it reuses the single source of truth for the backend set (do not re-list adapters here).

- [ ] **Step 8: Run to verify it passes**

Run (Windows): `py -3.11 -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_doctor_lists_all_backends -v`
Run (Linux/CI): `python -m pytest tests/scripts/test_alp_cli_model.py::test_alp_model_doctor_lists_all_backends -v`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add scripts/alp_model/adapters/ scripts/alp_cli/model.py tests/scripts/test_alp_model_adapters.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): 'alp model doctor' + adapter probe() availability report"
```

---

### Task 4: `alp model info NAME`

Decode a built `.alpmodel` and report its manifest (targets, per-blob `requires`) plus a SoM coverage matrix (declared backends × has-blob) when `--board` is given.

**Files:**
- Modify: `scripts/alp_cli/model.py` (new `info_cmd`)
- Test: `tests/scripts/test_alp_cli_model.py` (append)

**Interfaces:**
- Consumes: `alp_model.package.read_package()`; `alp_model.targets.resolve_targets(sku, *, metadata_root) -> list[TargetSpec]` (for the coverage matrix); `Manifest.to_dict()` (hex-encode `src_sha` via the existing `manifest._json_default`).
- Produces (the `info` payload):
  ```json
  {"name": "demo", "src_sha": "…hex…",
   "inputs": [...], "outputs": [...],
   "targets": [{"backend": "cpu", "silicon_ref": "*", "blob_format": "tflite",
                "accel_config": "", "arena": 0, "blob_bytes": 800,
                "requires": {"sram_kib": 0, "op_features": []}, "compiler_version": "passthrough"}],
   "skipped": [{"backend": "ethos_u", "accel_config": "ethos-u55-256", "status": "skipped", "reason": "…"}],
   "coverage_matrix": [{"backend": "cpu", "has_blob": true}, {"backend": "ethos_u", "has_blob": false}]}
  ```
  `coverage_matrix` is present only when `--board` (→ SKU) is supplied; `sram_kib` fit-vs-device-arena is intentionally NOT flagged here (the device NPU arena budget is a consumer/on-device concern — deferred, see spec §10).

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_alp_cli_model.py`:

```python
def test_alp_model_info_decodes_manifest_and_matrix(tmp_path):
    (tmp_path / "models").mkdir()
    shutil.copy(_ROOT / "tests/fixtures/models/tiny_int8.tflite",
                tmp_path / "models" / "m.tflite")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-AEN801\n"
        "cores: {}\n"
        "models:\n  - name: demo\n    source: models/m.tflite\n",
        encoding="utf-8")
    out = tmp_path / "out"
    CliRunner().invoke(cli, [
        "model", "build", "--board", str(tmp_path / "board.yaml"),
        "--out", str(out), "--metadata-root", str(_ROOT / "metadata"),
    ], catch_exceptions=False)
    result = CliRunner().invoke(cli, [
        "model", "info", "demo",
        "--out", str(out),
        "--board", str(tmp_path / "board.yaml"),
        "--metadata-root", str(_ROOT / "metadata"),
        "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 0, result.output
    info = _json.loads(result.output)
    assert info["name"] == "demo"
    assert any(t["backend"] == "cpu" for t in info["targets"])
    matrix = {row["backend"]: row["has_blob"] for row in info["coverage_matrix"]}
    assert matrix["cpu"] is True          # cpu always compiles
    assert "ethos_u" in matrix            # declared AEN801 backend appears in the matrix


def test_alp_model_info_missing_artifact_errors(tmp_path):
    result = CliRunner().invoke(cli, [
        "model", "info", "nope", "--out", str(tmp_path), "--format", "json",
    ], catch_exceptions=False)
    assert result.exit_code == 1
```

- [ ] **Step 2: Run to verify it fails**

Run (Windows): `py -3.11 -m pytest "tests/scripts/test_alp_cli_model.py::test_alp_model_info_decodes_manifest_and_matrix" "tests/scripts/test_alp_cli_model.py::test_alp_model_info_missing_artifact_errors" -v`
Expected: FAIL — `No such command 'info'`.

- [ ] **Step 3: Implement `info_cmd`**

Add to `scripts/alp_cli/model.py` (import: `from alp_model.targets import resolve_targets`). Reuse the public `Manifest.to_json()` (it hex-encodes `src_sha` already) rather than the private `_json_default`:

```python
@model_group.command(name="info", help="Decode a built .alpmodel: targets, requires, coverage matrix.")
@click.argument("name")
@click.option("--out", "out_dir", type=click.Path(path_type=Path),
              default=Path("build/models"), show_default=True, help="Build output directory.")
@click.option("--board", "board_path", type=click.Path(exists=True, path_type=Path),
              default=None, help="board.yaml — enables the SoM coverage matrix.")
@click.option("--metadata-root", type=click.Path(exists=True, path_type=Path),
              default=_DEFAULT_META, help="Path to the metadata/ root.")
@click.option("--format", "output_format", type=click.Choice(["human", "json"]),
              default="human", show_default=True)
def info_cmd(name: str, out_dir: Path, board_path: Path | None,
             metadata_root: Path, output_format: str) -> None:
    artifact = out_dir / f"{name}.alpmodel"
    if not artifact.is_file():
        click.echo(f"alp model info: no .alpmodel for '{name}' at {artifact}", err=True)
        raise SystemExit(1)
    mft, blobs = read_package(artifact.read_bytes())
    doc = json.loads(mft.to_json())        # public API; hex-encodes src_sha, keeps inputs/outputs/targets/coverage
    doc["targets"] = [{**t, "blob_bytes": len(blobs[t["blob"]])} for t in doc["targets"]]
    doc["skipped"] = doc.pop("coverage")
    doc.pop("v", None)
    if board_path is not None:
        sku = yaml.safe_load(board_path.read_text(encoding="utf-8"))["som"]["sku"]
        have = {t.backend for t in mft.targets}
        declared = {s.backend for s in resolve_targets(sku, metadata_root=metadata_root)}
        doc["coverage_matrix"] = [{"backend": b, "has_blob": b in have}
                                  for b in sorted(declared)]
    if output_format == "json":
        click.echo(json.dumps(doc, indent=2))
    else:
        click.echo(f"{doc['name']}: {len(doc['targets'])} blob(s), "
                   f"{len(doc['skipped'])} skipped")
        for t in doc["targets"]:
            click.echo(f"  {t['backend']:12} {t['blob_format']:12} {t['blob_bytes']} B")
```

- [ ] **Step 4: Run to verify it passes**

Run (Windows): `py -3.11 -m pytest "tests/scripts/test_alp_cli_model.py::test_alp_model_info_decodes_manifest_and_matrix" "tests/scripts/test_alp_cli_model.py::test_alp_model_info_missing_artifact_errors" -v`
Run (Linux/CI): `python -m pytest "tests/scripts/test_alp_cli_model.py::test_alp_model_info_decodes_manifest_and_matrix" "tests/scripts/test_alp_cli_model.py::test_alp_model_info_missing_artifact_errors" -v`
Expected: PASS.

- [ ] **Step 5: Full-suite regression + commit**

Run the whole model CLI + adapter suites:
`py -3.11 -m pytest tests/scripts/test_alp_cli_model.py tests/scripts/test_alp_model_adapters.py -v` (Linux/CI: `python -m pytest …`).
Expected: all PASS.

```bash
git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py
git commit -m "feat(model): 'alp model info' — decode .alpmodel + SoM coverage matrix"
```

---

## Final gate (before opening the PR)

Run the SDK's full local gate, not just the model tests (per running-local-ci):

```bash
bash scripts/test-all.sh
```

Expected: green on Windows + WSL Ubuntu. Then push `feat/alp-model-envelope` and open a PR to `dev` (labels/milestone per opening-github-prs-and-issues; no Claude footer).

## Self-review notes

- **Spec coverage:** implements spec §5 items 1 (`--format json`) + 2 (`list`/`info`/`doctor`). Item 3 (host manifest decoder) is satisfied by the already-existing `read_package()` — the spec's "reuse the canonical-JSON sidecar" was inaccurate (build writes no sidecar); `info` decodes the binary container directly. Items 4 (device runner) + 5 (cloud seam) are out of Plan A (Phase 3 / future).
- **Not covered here (later plans):** `tan model` upgrade from `WestForwardArgs` passthrough to an envelope-wrapping subcommand group (Plan B); the extension Models panel (Plan C). Plan B must repoint `tan`'s `Model(WestForwardArgs)` to a real subcommand set that spawns `alp model <sub> --format json` and wraps the payloads emitted here.
- **Type consistency:** payload keys match `manifest.py` field names (`backend`, `silicon_ref`, `blob_format`, `arena`, `requires`, `compiler_version`); `probe()` return shape is identical across the adapter overrides and the `doctor` payload.
