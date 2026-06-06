# Stage 2 Step 1 — `models: compile:` config/calibration plumbing — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Thread a per-backend, per-model **compile config** (DEEPX JSON+calibration, DRP-AI spec) from `board.yaml` `models[].compile:` through `build_model` into each adapter's `compile()`, and skip backends that *require* a config but have none with a clear `coverage: skipped ("no compile config")` — instead of guessing. Pure host Python; no vendor tools needed; fully testable now. (Step 2 = real `DeepxAdapter.compile()`, gated on the `dx-com` wheel; Steps 3-4 = runtime backends, bench-gated.)

**Architecture:** Grounding spec: `docs/superpowers/specs/2026-05-27-stage2-npu-compiler-integration-design.md` §3, §6. The `.alpmodel` contract (Stages 1a–1c) is frozen; this only extends the **input** `board.yaml models[]` schema + the adapter `compile()` signature. Compile-config block keys are the **backend ids** (`deepx_dxm1`, `drpai`) so `build_model` can do `compile_opts.get(spec.backend)` directly (the spec's `deepx:`/`drpai:` sketch is normalized to the canonical backend id).

**Tech Stack:** Python 3.10+ (`scripts/alp_model/`, `scripts/alp_cli/`), pytest, JSON Schema (`jsonschema`), Click `CliRunner`.

---

## File Structure
- **Modify** `scripts/alp_model/adapters/__init__.py` — `CompilerAdapter.compile()` gains `opts`; add `requires_compile_opts` class attr.
- **Modify** `scripts/alp_model/adapters/{cpu,ethos_u}.py` — `compile()` signature gains `opts` (ignored).
- **Modify** `scripts/alp_model/adapters/{deepx,drpai}.py` — set `requires_compile_opts = True`; `compile()` signature gains `opts`.
- **Modify** `scripts/alp_model/build.py` — `build_model(compile_opts=...)`; "no compile config" skip; pass `opts` to `compile()`.
- **Modify** `scripts/alp_cli/model.py` — read `m.get("compile")`, resolve path values relative to board.yaml, pass `compile_opts`.
- **Modify** `metadata/schemas/board.schema.json` — add `compile` to `models[].items.properties`.
- **Modify** tests: `test_alp_model_adapters.py`, `test_alp_model_build.py`, `test_board_models_schema.py`, `test_alp_cli_model.py`.

---

### Task 1: Adapter interface + adapters — thread `opts`, mark `requires_compile_opts`

**Files:** `scripts/alp_model/adapters/__init__.py`, `cpu.py`, `ethos_u.py`, `deepx.py`, `drpai.py`; test `tests/scripts/test_alp_model_adapters.py`

- [ ] **Step 1: Failing tests** — append to `tests/scripts/test_alp_model_adapters.py`:

```python
def test_cpu_and_vela_do_not_require_compile_opts():
    assert CpuAdapter().requires_compile_opts is False
    assert VelaAdapter().requires_compile_opts is False


def test_drpai_and_deepx_require_compile_opts():
    assert DrpaiAdapter().requires_compile_opts is True
    assert DeepxAdapter().requires_compile_opts is True


def test_cpu_compile_accepts_opts_kwarg(tmp_path):
    src = tmp_path / "m.tflite"; src.write_bytes(b"TFL3-X")
    blob = CpuAdapter().compile(src, accel_config="", out_dir=tmp_path, opts=None)
    assert blob.payload == b"TFL3-X"


def test_vela_compile_accepts_opts_kwarg(tmp_path, monkeypatch):
    src = tmp_path / "m.tflite"; src.write_bytes(b"TFL3-X")
    def fake_run(cmd, capture_output, text, timeout):
        (tmp_path / "m_vela.tflite").write_bytes(b"VELA-OUT")
        class _R: returncode = 0; stdout = ""; stderr = ""
        return _R()
    monkeypatch.setattr("alp_model.adapters.ethos_u.subprocess.run", fake_run)
    blob = VelaAdapter().compile(src, accel_config="ethos-u55-128",
                                 out_dir=tmp_path, opts={"ignored": True})
    assert blob.payload == b"VELA-OUT"
```

- [ ] **Step 2: Run → fail** — `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py -q` (AttributeError / unexpected-kwarg).

- [ ] **Step 3: Implement.** In `adapters/__init__.py`, replace the `CompilerAdapter` body:

```python
class CompilerAdapter(ABC):
    backend: str                # cpu | ethos_u | drpai | deepx_dxm1
    # True for backends that need a per-model compile config the SDK can't
    # derive (DEEPX JSON+calibration, DRP-AI spec). build_model records a
    # "no compile config" coverage skip for these when no opts block is given.
    requires_compile_opts: bool = False

    @abstractmethod
    def is_available(self) -> bool:
        """True if this backend's compiler is installed/usable on this host."""

    @abstractmethod
    def accepts(self, src_format: str) -> bool:
        """True if this adapter can consume the given source format (onnx|tflite)."""

    @abstractmethod
    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None) -> Blob:
        """Compile @source for @accel_config; return the Blob.

        @opts is the per-model compile config for this backend
        (board.yaml `models[].compile.<backend>`), with any path values already
        resolved to absolute paths by the caller; None when the backend needs
        no per-model config (cpu, ethos_u)."""
```

In `cpu.py` and `ethos_u.py`, change each `def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:` to add `, opts: dict | None = None` before `)`. Bodies unchanged (opts ignored).

In `drpai.py` and `deepx.py`, add `requires_compile_opts = True` under the `backend = ...` line, and change `def compile(self, source: Path, *, accel_config: str, out_dir: Path) -> Blob:` to `def compile(self, source: Path, *, accel_config: str, out_dir: Path, opts: dict | None = None) -> Blob:` (body still `raise NotImplementedError("real ... compile lands in Stage 2")`).

- [ ] **Step 4: Run → pass.** `py -3.14 -m pytest tests/scripts/test_alp_model_adapters.py -q` (all pass — existing detect-and-skip tests still call `compile(...)` without opts, which is fine since opts defaults to None).

- [ ] **Step 5: Commit** (stage, then bare commit — separate calls):
`git add scripts/alp_model/adapters tests/scripts/test_alp_model_adapters.py`
`git commit -q -m "feat(alpmodel): adapter compile() gains opts + requires_compile_opts"`

---

### Task 2: `build_model` threads `compile_opts` + "no compile config" skip

**Files:** `scripts/alp_model/build.py`; test `tests/scripts/test_alp_model_build.py`

- [ ] **Step 1: Failing tests** — append to `tests/scripts/test_alp_model_build.py`:

```python
def test_build_model_skips_backend_missing_compile_config(tmp_path):
    # An adapter that requires compile opts + none provided -> coverage skip, compile() never called.
    class _NeedsOpts(CompilerAdapter):
        backend = "drpai"
        requires_compile_opts = True
        def is_available(self): return True
        def accepts(self, src_format): return src_format == "onnx"
        def compile(self, source, *, accel_config, out_dir, opts=None):
            raise AssertionError("must not compile without opts")
    src = tmp_path / "m.onnx"; src.write_bytes(b"ONNX")
    out = build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                      metadata_root=_META, adapters=[CpuAdapter(), _NeedsOpts()])
    mft, _ = read_package(out.read_bytes())
    skips = [c for c in mft.coverage if c.backend == "drpai"]
    assert skips and all(c.status == "skipped" and "no compile config" in c.reason for c in skips)


def test_build_model_passes_compile_opts_to_adapter(tmp_path):
    seen = {}
    class _Capture(CompilerAdapter):
        backend = "drpai"
        requires_compile_opts = True
        def is_available(self): return True
        def accepts(self, src_format): return src_format == "onnx"
        def compile(self, source, *, accel_config, out_dir, opts=None):
            seen["opts"] = opts
            return Blob(format="drpai_dir", payload=b"RT", arena_bytes=0)
    src = tmp_path / "m.onnx"; src.write_bytes(b"ONNX")
    build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                metadata_root=_META, adapters=[CpuAdapter(), _Capture()],
                compile_opts={"drpai": {"spec": "/abs/p.yaml"}})
    assert seen["opts"] == {"spec": "/abs/p.yaml"}
```

Add `Blob` to the import line: `from alp_model.adapters import CompilerAdapter, Blob`.

- [ ] **Step 2: Run → fail** — unexpected kwarg `compile_opts`.

- [ ] **Step 3: Implement.** In `build.py`, change the signature to add `compile_opts`:

```python
def build_model(*, sku: str, name: str, source: Path, out_dir: Path,
                metadata_root: Path,
                adapters: list[CompilerAdapter] | None = None,
                compile_opts: dict[str, dict] | None = None) -> Path:
```

After `src_fmt = _src_format(source)` add `opts_by_backend = compile_opts or {}`. In the per-spec loop, right after the `adapter is None` skip, insert:

```python
        backend_opts = opts_by_backend.get(spec.backend)
        if adapter.requires_compile_opts and not backend_opts:
            coverage.append(Coverage(spec.backend, spec.accel_config, "skipped",
                                     f"no compile config for {spec.backend} "
                                     f"(add models[].compile.{spec.backend} to board.yaml)"))
            continue
```

Change the compile call to pass opts:
`blob = adapter.compile(source, accel_config=spec.accel_config, out_dir=out_dir, opts=backend_opts)`

- [ ] **Step 4: Run → pass** — `py -3.14 -m pytest tests/scripts/test_alp_model_build.py -q` (incl. existing tests: the existing `_Unavail` test's adapter is unavailable so its old-signature `compile` is never called; the default-registry V2M101 test now skips drpai/deepx with "no compile config" since no opts are passed — UPDATE that test's assertion: `assert any("no compile config" in c.reason or "not installed" in c.reason for c in ...)` is NOT needed; drpai/deepx now skip on the requires-opts check BEFORE is_available, so the reason becomes "no compile config"; adjust `test_build_model_v2m101_records_drpai_and_deepx_skips` to assert drpai+deepx are in `skipped` (reason text no longer asserted, or assert "no compile config")).

- [ ] **Step 5: Commit** — `git add scripts/alp_model/build.py tests/scripts/test_alp_model_build.py` then bare `git commit -q -m "feat(alpmodel): build_model threads compile_opts; 'no compile config' skip"`

---

### Task 3: `board.yaml models[].compile:` schema + tests

**Files:** `metadata/schemas/board.schema.json`; test `tests/scripts/test_board_models_schema.py`

- [ ] **Step 1: Failing tests** — append to `tests/scripts/test_board_models_schema.py`:

```python
def test_models_compile_block_validates():
    jsonschema.validate([{
        "name": "person_detect", "source": "models/p.onnx",
        "compile": {
            "deepx_dxm1": {"config": "models/p.deepx.json", "calibration": "models/calib/"},
            "drpai": {"spec": "models/p.drpai.yaml"},
        },
    }], _MODELS_SCHEMA)


def test_models_compile_rejects_unknown_backend_key():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.onnx",
                              "compile": {"vela": {"x": 1}}}], _MODELS_SCHEMA)


def test_models_compile_deepx_requires_config_and_calibration():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.onnx",
                              "compile": {"deepx_dxm1": {"config": "c.json"}}}], _MODELS_SCHEMA)
```

- [ ] **Step 2: Run → fail** — the `compile` key is rejected (`additionalProperties: false`).

- [ ] **Step 3: Implement.** In `metadata/schemas/board.schema.json`, in the `models.items.properties` object (after the `inputs` property), add a `compile` property:

```json
          "compile": {
            "type": "object",
            "additionalProperties": false,
            "description": "Per-backend compile configuration for NPU toolchains that need a per-model config + calibration the SDK cannot derive (DRP-AI, DEEPX). Keys are backend ids; a backend with no block here is recorded as a coverage skip (\"no compile config\") rather than guessed. Paths are relative to this board.yaml.",
            "properties": {
              "deepx_dxm1": {
                "type": "object",
                "additionalProperties": false,
                "required": ["config", "calibration"],
                "properties": {
                  "config":      { "type": "string", "description": "Path to the dxcom JSON config (per-model quantization/compile settings)." },
                  "calibration": { "type": "string", "description": "Path to the calibration dataset directory for post-training quantization." }
                }
              },
              "drpai": {
                "type": "object",
                "additionalProperties": false,
                "required": ["spec"],
                "properties": {
                  "spec": { "type": "string", "description": "Path to the DRP-AI TVM compile spec (model input/quant configuration)." }
                }
              }
            }
          }
```

(Add a comma after the `inputs` property's closing `}` so JSON stays valid.)

- [ ] **Step 4: Run → pass** — `py -3.14 -m pytest tests/scripts/test_board_models_schema.py -q`. Also `py -3.14 -c "import json; json.load(open('metadata/schemas/board.schema.json'))"` (valid JSON).

- [ ] **Step 5: Commit** — `git add metadata/schemas/board.schema.json tests/scripts/test_board_models_schema.py` then bare `git commit -q -m "feat(schema): board.yaml models[].compile per-backend config block"`

---

### Task 4: CLI threads `compile:` (paths resolved) into `build_model`

**Files:** `scripts/alp_cli/model.py`; test `tests/scripts/test_alp_cli_model.py`

- [ ] **Step 1: Failing test** — append to `tests/scripts/test_alp_cli_model.py`:

```python
def test_alp_model_build_threads_compile_opts(tmp_path, monkeypatch):
    # CLI must read models[].compile, resolve its paths relative to board.yaml,
    # and pass them to build_model as compile_opts.
    (tmp_path / "models").mkdir()
    (tmp_path / "models" / "m.onnx").write_bytes(b"ONNX")
    (tmp_path / "models" / "m.deepx.json").write_text("{}", encoding="utf-8")
    (tmp_path / "board.yaml").write_text(
        "name: demo\n"
        "som:\n  sku: E1M-V2M101\n"
        "cores: {}\n"
        "models:\n"
        "  - name: demo\n"
        "    source: models/m.onnx\n"
        "    compile:\n"
        "      deepx_dxm1: { config: models/m.deepx.json, calibration: models/ }\n",
        encoding="utf-8")
    captured = {}
    import alp_cli.model as climod
    def fake_build_model(*, sku, name, source, out_dir, metadata_root, compile_opts=None):
        captured["compile_opts"] = compile_opts
        p = out_dir / f"{name}.alpmodel"; out_dir.mkdir(parents=True, exist_ok=True); p.write_bytes(b"X")
        return p
    monkeypatch.setattr(climod, "build_model", fake_build_model)
    from click.testing import CliRunner
    from alp_cli.main import cli
    res = CliRunner().invoke(cli, ["model", "build", "--board", str(tmp_path / "board.yaml"),
                                   "--out", str(tmp_path / "out"),
                                   "--metadata-root", str(_ROOT / "metadata")],
                             catch_exceptions=False)
    assert res.exit_code == 0, res.output
    opts = captured["compile_opts"]["deepx_dxm1"]
    assert Path(opts["config"]).is_absolute() and opts["config"].endswith("m.deepx.json")
    assert Path(opts["calibration"]).is_absolute()
```

- [ ] **Step 2: Run → fail** — `compile_opts` not passed (KeyError / None).

- [ ] **Step 3: Implement.** In `scripts/alp_cli/model.py`, add a resolver helper above `build_cmd`:

```python
def _resolve_compile(block: dict | None, base: Path) -> dict | None:
    """Resolve every path value in each per-backend compile block to an absolute
    path relative to the board.yaml dir (all current opts values are paths)."""
    if not block:
        return None
    return {
        backend: {k: str((base / v).resolve()) if isinstance(v, str) else v
                  for k, v in (opts or {}).items()}
        for backend, opts in block.items()
    }
```

In the `for m in models:` loop, change the `build_model(...)` call to:

```python
        out = build_model(sku=sku, name=m["name"], source=source,
                          out_dir=out_dir, metadata_root=metadata_root,
                          compile_opts=_resolve_compile(m.get("compile"), base))
```

- [ ] **Step 4: Run → pass** — `py -3.14 -m pytest tests/scripts/test_alp_cli_model.py -q` (incl. existing CLI tests — models without `compile` pass `compile_opts=None`, unchanged behavior).

- [ ] **Step 5: Commit** — `git add scripts/alp_cli/model.py tests/scripts/test_alp_cli_model.py` then bare `git commit -q -m "feat(cli): thread board.yaml models[].compile into build_model"`

---

### Task 5 (Step 2 — GATED ATTEMPT): real `DeepxAdapter.compile()` via `dxcom`

> Needs the proprietary `dx-com` wheel (`dx_com-2.3.0`, Linux py3.8–3.12) from the private `alp-sdk-internal` repo (`vendors/deepx-dxm1/dx-com/`, Git LFS). Install in a **WSL** venv. If the wheel/LFS is unavailable, STOP after the mocked test + a `skipif` real test, and record the blocker — do not fake `dxcom`.

- [ ] **Step 1:** In WSL, `git -C <alp-sdk-internal> lfs pull` the wheel; create a py3.12 venv; `pip install dx_com-2.3.0-*.whl`; run `dxcom --help` and capture the real CLI flags + output structure (dir vs single `.dxnn`).
- [ ] **Step 2:** Implement `DeepxAdapter.compile(source, *, accel_config, out_dir, opts)`: require `opts` (raise if None), shell out `dxcom -m <source> -c <opts['config']> -o <out_dir>` (+ calibration per the confirmed CLI), read back the artifact, return `Blob(format=<dxnn|deepx_dir>, payload=..., compiler_version="dx-com <ver>")`. Mirror `VelaAdapter`'s timeout + returncode + missing-output handling.
- [ ] **Step 3:** Tests mirroring Vela: a **mocked** test (monkeypatch `subprocess.run` to emit a fake artifact, assert cmd + Blob) and a **`@pytest.mark.skipif(shutil.which("dxcom") is None)`** real test compiling a tiny ONNX fixture. Settle `blob_format` (extend `Blob.format`/`Target.blob_format` docs + `manifest._TARGET_KEYS` unaffected; add `deepx_dir` to the format docstrings if the output is a dir).
- [ ] **Step 4:** Commit; update the Stage-2 spec §5 row (DeepxAdapter.compile → DONE) + CHANGELOG.

### Steps 3-4 (GATED — not this cycle)
`DrpaiAdapter.compile()` (open DRP-AI TVM build) and the Yocto runtime backends (`inference_deepx.cpp` via `dx_rt`, new `inference_drpai.cpp`, `meta-alp` recipes) need the licensed `dx_rt` SDK (C++ API not public) + bench DX-M1 / RZ-V2N silicon. Out of scope; tracked by issues #58/#59 and the Stage-2 spec.

---

## Local CI before merge to `dev`
- `py -3.14 -m pytest tests/scripts -q` (Windows) **and** WSL `python3 -m pytest tests/scripts -q`.
- `py -3.14 scripts/alp_cli/main.py validate` (or the metadata-validate sweep) — schema changed.
- `py -3.14 scripts/check_doc_drift.py` — docs/CHANGELOG edits stay clean.
- No C/H/examples touched ⇒ no twister.
