# `tan model` envelope-wrapping — Implementation Plan (Plan B, tan-cli)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. **Route implementation to the `tan-implementor` agent (Rust); review with `alp-reviewer`.**

**Goal:** Upgrade `tan model {build,list,info,doctor}` from a raw inherited-stdio passthrough into a real envelope-wrapping command: in `--format json` mode, capture the SDK's `python -m alp_cli model <sub> --format json` payload (Plan A) and wrap it in tan's `{command, ok, exitCode, project, data, issues}` envelope. Text mode and the interactive/long-running subs keep streaming unchanged.

**Architecture:** A new `crates/tan-cli/src/commands/model.rs` owns `tan model`. `main.rs` dispatches `Command::Model` to it. In `--format json` with a *wrappable* first token (`build`/`list`/`info`/`doctor`), it spawns `python -m alp_cli model <sub> <passthrough> --format json` **captured** (not inherited), parses stdout as `serde_json::Value`, and emits `Envelope::new("model", project, data, issues, exit)`. Every other case — text mode, an unknown/interactive sub (`run`, help), a pre-spawn guard failure — delegates to the existing streaming forwarder `sdk_cli::run` (long compiles must stream live; only the machine/JSON path captures). No rename: `alp_cli` stays the SDK backend (`pyproject.toml:49-50`); `tan` is only the surface.

**Tech Stack:** Rust (edition 2024, rust ≥1.85), `clap` 4.6, `serde_json` (workspace, `preserve_order`), the crate's `Envelope`/`ExitCode`/`CommandRun`. Gates: `cargo fmt --all --check` · `cargo clippy --all-targets -- -D warnings` · `cargo build --all-targets` · `cargo test` (all four).

## Global Constraints

- **tan-cli is a PRIVATE draft repo: commit directly to `main`.** NOT the alp-sdk feat-branch/PR flow. No Claude/AI attribution in commits.
- **The four cargo gates are the bar** — all green before push. `rustc ≥ 1.85` (edition 2024).
- **Keep files small; pure logic is unit-testable without spawning.** The argv builder, wrappable-sub detection, exit mapping, and envelope-wrap are pure functions with unit tests; only `run` does IO.
- **Reuse `sdk_cli`'s spawn primitives — do not duplicate the SDK resolution / PYTHONPATH / python-too-old guard.** Make the needed helpers `pub(crate)` in `sdk_cli.rs` (or factor a shared `spawn` helper) rather than copy them.
- **SDK-contract strings verbatim:** `alp_cli`, `python -m alp_cli`, `board.yaml`, `--format json`. Only the user-facing command is `tan`.
- **Wrapping happens ONLY in `--format json` mode.** Text mode always streams (inherited stdio) so a multi-minute `vela`/`dxcom` compile shows live progress. This mirrors how the extension consumes tan (always `--format json`) vs a human at the terminal.
- **`data` is the SDK payload passed through verbatim** (`serde_json::Value`) — tan does NOT model each per-sub payload struct (build `{models:[…]}`, doctor `{toolchains:[…]}`, …). The Plan-A payload becomes the envelope's `data` unchanged.

---

### Task 1: Pure model-command helpers (argv, wrappable-sub, exit map, envelope wrap)

Pure functions in `commands/model.rs`, unit-tested without spawning. These encode every decision; Task 2's `run` is thin IO orchestration over them.

**Files:**
- Create: `crates/tan-cli/src/commands/model.rs` (helpers + `#[cfg(test)] mod tests`)

**Interfaces (Produces — Task 2 consumes these):**
- `const WRAPPABLE_SUBS: [&str; 4] = ["build", "list", "info", "doctor"];`
- `fn wrappable_sub(passthrough: &[String]) -> Option<&str>` — returns the first token iff it is one of `WRAPPABLE_SUBS` (the sub whose `--format json` payload tan can wrap); `None` otherwise (empty, `run`, `--help`, unknown).
- `fn model_argv(passthrough: &[String], json: bool) -> Vec<String>` — builds the `alp_cli` argv tail: `["-m", "alp_cli", "model", <passthrough…>]`, and when `json` is true appends `["--format", "json"]` **unless** the passthrough already contains a `--format` token (dedup — the user may have typed it after `model`).
- `fn map_model_exit(child_code: Option<i32>) -> ExitCode` — `Some(0) → Success`; `Some(_) → RuntimeFailure`; `None` (spawn produced no code) → `RuntimeFailure`.
- `fn wrap_model_json(stdout: &str, stderr: &str, exit: ExitCode, project: Project) -> String` — parse `stdout` as `serde_json::Value` for the envelope `data`; on a parse error, `data` is `serde_json::Value::Null` and an issue `model.bad-payload` is added. On a non-`Success` `exit`, add one issue `model.failed` whose message is the trimmed `stderr` (or a generic line when stderr is empty). Returns `Envelope::new("model", project, data, issues, exit.code()).to_json()`.

- [ ] **Step 1: Write the failing tests**

In `crates/tan-cli/src/commands/model.rs` `#[cfg(test)] mod tests`:

```rust
#[test]
fn wrappable_sub_detects_the_four_and_rejects_others() {
    assert_eq!(wrappable_sub(&["build".into(), "--board".into(), "b.yaml".into()]), Some("build"));
    assert_eq!(wrappable_sub(&["doctor".into()]), Some("doctor"));
    assert_eq!(wrappable_sub(&["run".into(), "m".into()]), None); // Phase-3, streams
    assert_eq!(wrappable_sub(&["--help".into()]), None);
    assert_eq!(wrappable_sub(&[]), None);
}

#[test]
fn model_argv_appends_format_json_once() {
    let a = model_argv(&["build".into(), "--board".into(), "b.yaml".into()], true);
    assert_eq!(a, vec!["-m", "alp_cli", "model", "build", "--board", "b.yaml", "--format", "json"]);
    // dedup: user already passed --format
    let b = model_argv(&["build".into(), "--format".into(), "json".into()], true);
    assert_eq!(b.iter().filter(|s| *s == "--format").count(), 1);
    // text mode: no --format appended
    let c = model_argv(&["build".into()], false);
    assert_eq!(c, vec!["-m", "alp_cli", "model", "build"]);
}

#[test]
fn map_model_exit_maps_success_and_failure() {
    assert_eq!(map_model_exit(Some(0)), ExitCode::Success);
    assert_eq!(map_model_exit(Some(1)), ExitCode::RuntimeFailure);
    assert_eq!(map_model_exit(None), ExitCode::RuntimeFailure);
}

#[test]
fn wrap_model_json_passes_payload_through_as_data() {
    let payload = r#"{"models":[{"name":"demo","targets":[]}]}"#;
    let doc = wrap_model_json(payload, "", ExitCode::Success, Project { root: None, board_yaml: None });
    let v: serde_json::Value = serde_json::from_str(&doc).unwrap();
    assert_eq!(v["command"], "model");
    assert_eq!(v["ok"], true);
    assert_eq!(v["exitCode"], 0);
    assert_eq!(v["data"]["models"][0]["name"], "demo");
    assert_eq!(v["issues"].as_array().unwrap().len(), 0);
}

#[test]
fn wrap_model_json_reports_failure_and_bad_payload() {
    // non-success exit → one model.failed issue carrying stderr
    let doc = wrap_model_json("", "no blob compiled", ExitCode::RuntimeFailure,
                              Project { root: None, board_yaml: None });
    let v: serde_json::Value = serde_json::from_str(&doc).unwrap();
    assert_eq!(v["ok"], false);
    assert_eq!(v["exitCode"], 1);
    assert_eq!(v["issues"][0]["code"], "model.failed");
    assert!(v["issues"][0]["message"].as_str().unwrap().contains("no blob compiled"));
    // unparseable stdout on a "success" exit → model.bad-payload + null data
    let doc2 = wrap_model_json("not json", "", ExitCode::Success,
                               Project { root: None, board_yaml: None });
    let v2: serde_json::Value = serde_json::from_str(&doc2).unwrap();
    assert_eq!(v2["issues"][0]["code"], "model.bad-payload");
    assert!(v2["data"].is_null());
}
```

- [ ] **Step 2: Run the tests to verify they fail** — `cargo test -p tan-cli model::` → FAIL (functions undefined).

- [ ] **Step 3: Implement the four pure helpers** in `commands/model.rs`, mirroring `sdk_cli::build_argv`'s dedup style (`json && !passthrough.iter().any(|a| a == "--format")`) and `validate::to_cli_issues`'s issue-synthesis style. Import `crate::envelope::{Envelope, Issue, Project}` and `crate::exit::ExitCode`. `data` is built with `serde_json::from_str::<serde_json::Value>(stdout)`.

- [ ] **Step 4: Run the tests to verify they pass** — `cargo test -p tan-cli model::` → PASS.

- [ ] **Step 5: Gate + commit**

```bash
cargo fmt --all -- --check && cargo clippy --all-targets -- -D warnings && cargo test -p tan-cli model::
git add crates/tan-cli/src/commands/model.rs
git commit -m "feat(model): pure helpers for tan model envelope wrapping"
```

---

### Task 2: `model::run` orchestration + wiring

Thin IO layer: capture-and-wrap in JSON mode for a wrappable sub, else delegate to the streaming forwarder. Reuse `sdk_cli`'s SDK-resolution / PYTHONPATH / python-guard rather than re-deriving them.

**Files:**
- Modify: `crates/tan-cli/src/commands/model.rs` (add `pub fn run`)
- Modify: `crates/tan-cli/src/commands/sdk_cli.rs` — make the spawn primitives reusable: change `default_python_binary`, `build_pythonpath`, and the SDK-root/PYTHONPATH/python-guard setup into a `pub(crate)` helper, e.g. `pub(crate) fn prepare_alp_cli(g) -> Result<AlpCliSpawn, CommandRun>` returning the resolved `{python, sdk_root, pythonpath, workspace_root}` (or the guard's failing `CommandRun`). `sdk_cli::run` is refactored to call it too — one code path, no duplication.
- Modify: `crates/tan-cli/src/commands/mod.rs` — add `pub mod model;`
- Modify: `crates/tan-cli/src/main.rs:65` — `Command::Model(args) => commands::model::run(&global, &args.args),`

**Interfaces:**
- Consumes (Task 1): `wrappable_sub`, `model_argv`, `map_model_exit`, `wrap_model_json`.
- Consumes (sdk_cli): the new `pub(crate) prepare_alp_cli` + the existing streaming `sdk_cli::run`.
- Produces: `pub fn run(g: &GlobalArgs, passthrough: &[String]) -> CommandRun`.

`run` logic (exact behavior):
```
if !g.is_json()  ||  wrappable_sub(passthrough).is_none():
    return sdk_cli::run(g, "model", passthrough)   // stream (text mode, run, help, unknown)
// JSON + wrappable → capture + wrap
match prepare_alp_cli(g) {
    Err(guard_run) => return guard_run,            // unresolved SDK / python-too-old → its own json envelope
    Ok(spawn) => {
        let argv = model_argv(passthrough, /*json=*/ true);
        let output = Command::new(spawn.python).args(argv)
            .current_dir(spawn.workspace_root).env("ALP_SDK_ROOT", spawn.sdk_root)
            .env("PYTHONPATH", spawn.pythonpath).output();
        match output {
            Ok(out) => {
                let exit = map_model_exit(out.status.code());
                let json = wrap_model_json(&stdout, &stderr, exit, project_from(g));  // project best-effort
                CommandRun { exit, text: Vec::new(), json: Some(json) }
            }
            Err(e) => /* launch_error → a model.failed json envelope, exit RuntimeFailure */
        }
    }
}
```

`project_from(g)`: best-effort — reuse `resolve_cli_project_context(g)` to fill `Project { root, board_yaml }` (build/list/info are board-scoped); acceptable to pass `Project { None, None }` for v1 if resolution is awkward — **note which** in the code comment. (doctor is not board-scoped; a null project there is correct.)

- [ ] **Step 1: Write the failing tests** (non-spawn branches only — a real `python -m alp_cli` spawn is environment-dependent and out of scope for unit tests, matching `sdk.rs`/`sdk_cli.rs` which test guards + pure logic, not live spawns):

```rust
fn json_global() -> GlobalArgs { /* …Format::Json, all else default (see sdk_cli.rs tests)… */ }
fn text_global() -> GlobalArgs { /* …Format::Text… */ }

#[test]
fn text_mode_delegates_to_streaming_forwarder() {
    // text mode must NOT capture/wrap: json is None (streamed), regardless of sub.
    let run = run(&text_global(), &["doctor".into()]);
    assert!(run.json.is_none());
}

#[test]
fn json_non_wrappable_sub_delegates_and_does_not_wrap_as_model_payload() {
    // `run` is Phase-3 / interactive → still streams even under --format json
    // (sdk_cli emits its own forward envelope only on a guard failure, never a model payload).
    let out = run(&json_global(), &["run".into(), "m".into()]);
    // no model-payload capture attempted; behavior identical to sdk_cli::run for this sub
    assert!(out.json.is_none() || !out.json.as_ref().unwrap().contains("\"command\":\"model\"") ||
            out.exit != ExitCode::Success); // documents: not a wrapped model payload
}
```
(If the guard-failure path is easier to assert deterministically — e.g. `--sdk-root /nonexistent` under `--format json` for a wrappable sub yields a `model`/forward failure envelope with `ok:false` — prefer that as the second test; it exercises `prepare_alp_cli`'s failure branch without a real python spawn.)

- [ ] **Step 2: Run to verify they fail** — `cargo test -p tan-cli model::` → FAIL (`run` undefined / not wired).

- [ ] **Step 3: Implement** `prepare_alp_cli` (refactor in `sdk_cli.rs`), `model::run`, and the `mod.rs` + `main.rs:65` wiring. Refactor `sdk_cli::run` to consume `prepare_alp_cli` so the streaming path and the capture path share one resolution.

- [ ] **Step 4: Run to verify they pass** — `cargo test -p tan-cli` → PASS (model + the refactored sdk_cli tests both green).

- [ ] **Step 5: Full gate + commit**

```bash
cargo fmt --all -- --check && cargo clippy --all-targets -- -D warnings && cargo build --all-targets && cargo test
git add crates/tan-cli/src/commands/model.rs crates/tan-cli/src/commands/sdk_cli.rs crates/tan-cli/src/commands/mod.rs crates/tan-cli/src/main.rs
git commit -m "feat(model): tan model wraps alp_cli --format json payload in the tan envelope"
```

---

## Final gate (before push)

All four cargo gates from the tan-cli root, green:
```bash
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
cargo build --all-targets
cargo test
```
Then push `main` (draft-repo convention — direct commit, no PR). Optional manual e2e where an SDK checkout resolves: `tan model doctor --format json --sdk-root <alp-sdk>` → a parseable `{command:"model",ok:true,data:{toolchains:[…]}}` envelope.

## Self-review notes

- Implements spec §6 item 1 (upgrade the `tan model` passthrough to envelope-wrapping). Items 2 (`contractVersion` guard) + 3 (`run` transport) are deferred: item 2 pairs with the SDK emitting a version field (not in Plan A's payloads yet); item 3 is Phase 3 (bench-gated).
- **Capture-only-in-JSON** is the load-bearing decision: it preserves live streaming for multi-minute `vela`/`dxcom` compiles in text mode while giving the extension the parseable envelope it needs.
- **`data` = the SDK payload verbatim** keeps the contract single-sourced — the Plan-A payload shapes (`{models}`, `{toolchains}`) are defined once in alp-sdk; tan wraps, never re-models them.
- No rename of `alp_cli` (SDK backend, `python -m alp_cli`); only the user surface is `tan`.
