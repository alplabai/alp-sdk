# VS Code Models panel — Implementation Plan (Plan C, alp-sdk-vscode)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax. **Route implementation to a `general-purpose` or bounded `alp-implementor` agent WITH the `alp-sdk-vscode` skill (TypeScript); review with `alp-reviewer`.**

**Goal:** Add a **Models** GUI surface to the extension that shells the envelope-emitting `tan model {list,info,doctor,build}` (Plans A+B) and renders it: per-model artifact status + backend-coverage, NPU-toolchain availability, a build action with progress, and a `.alpmodel` inspector. Model *editing* reuses the existing Configurator "AI models" card. Plus the LSP `models` field-docs thin-win.

**Architecture:** A new separate webview panel (`data-alp-mode` = `models`), mirroring `hardware-explorer`: `src/models/panel.ts` (extension host) whose `refresh()`/message-handler calls `runAlpCommand(context, ["model", …])`, parses the `AlpEnvelope`, and posts a `ModelsDataMessage`; `packages/alp-webview/src/features/models/` (React view + hook) renders it. A long `tan model build` mirrors the `sendProgress`/`sdkInstallProgress` flow. **Thin extension:** all model logic stays in `tan`/`alp_cli`; this repo only shells + renders (no build/plan logic in TS).

**Tech Stack:** TypeScript (strict), React 19 + Vite webview, `node --test`. Gates: `pnpm run compile` (tsc + `vp build`) + `pnpm test`.

## Global Constraints

- **PUBLIC repo.** No secrets, no login-gated vendor links (the toolchain doctor must *guide*, never link a `dxcom` download), no absolute local paths in committed files, no Claude/AI attribution. "Alp Lab" not "ALP Lab".
- **Keep the extension THIN.** It shells `tan` + parses the envelope; it must NOT re-implement build/plan/model logic in TypeScript. The `data` payloads are the SDK's (Plan A) — render them, don't re-model their semantics.
- **The message protocol is mirrored MANUALLY** in both `src/ideHub/messages.ts` and `packages/alp-webview/src/types.ts` — every new message goes in BOTH, in lockstep, and into BOTH `ExtToWebviewMessage`/`WebviewToExtMessage` unions.
- **The envelope entry point is `runAlpCommand(context, args, cwd?, {signal?})`** (`src/alpCli/vscodeAdapter.ts:589`), which appends `--format json` itself and returns `{ outcome, raw }` where `outcome.envelope: AlpEnvelope<T> | null` = `{command, ok, exitCode, project, data, issues}`. Never spawn `tan` by hand.
- **Node ≥ 22.13 to build** (`pnpm` 11). This machine defaults to Node v20 — use the portable-Node-24 workaround (see `reference_vscode_node_toolchain` memory: fetch the portable zip, prepend to PATH for the session) before running any `pnpm`. Windows has **~13 pre-existing path-separator test failures** (POSIX `/` vs `\`) that are NOT regressions — gate criterion is **`pnpm run compile` green + this task's own tests green + NO NEW failures beyond that baseline** (CI on Linux is authoritative).
- **Runtime dependency (not a code gate):** the panel calls the *installed* `tan`; the live round-trip needs the envelope-wrapping `tan model` (Plan B) released + a `tan` that resolves it, and `alp model` (Plan A, PR #907) merged. Until then this panel compiles + unit-tests against the contract but cannot round-trip live. Do NOT block the plan on it; DO surface a clean "update tan" message when the envelope is absent/old.

## Reference patterns (mirror these — do not invent new shapes)

- Panel host: `src/hardwareExplorer/panel.ts:17-133` (`HardwareExplorerPanel`, `buildWebviewHtml(webview, extensionUri, "<mode>")`).
- Envelope caller: `src/ideHub/sdkManagerMessages.ts:151-170` (`runAlpCommand(context,["sdk","list"])` → `outcome.envelope.data`).
- Progress: `src/ideHub/sdkManagerMessages.ts:205-247` (`sendProgress(log,done,success?)` → `sdkInstallProgress`, inside `vscode.window.withProgress`).
- Message types: `HardwareExplorerDataMessage` (`messages.ts:159`) + mirror (`types.ts:387`); `RequestSdkReleasesMessage` (`messages.ts:287`) + the request switch (`sdkManagerMessages.ts:250-274`).
- Webview feature: `packages/alp-webview/src/features/hardware-explorer/` (`View.tsx` + `useHardwareExplorer.ts` + `index.ts`) + `App.tsx:68` Router case.
- Command reg: `package.json:221-225` + `src/extension.ts:117-119` (`alp.openHardwareExplorer`).
- Editing reuse: Configurator "AI models" card `ConfiguratorView.tsx:1414-1538` + `ModelEntry` (`packages/alp-core/src/board/models.ts:162-172`).

---

### Task 1: Message contract (`ModelsData` / `RequestModels` / `ModelBuildProgress`)

Add the three message types on BOTH sides, in the unions. Pure protocol; verified by `pnpm run compile`.

**Files:**
- Modify: `src/ideHub/messages.ts` — add the three interfaces + add each to `ExtToWebviewMessage` / `WebviewToExtMessage`.
- Modify: `packages/alp-webview/src/types.ts` — mirror all three + the union membership.

**Interfaces (exact shapes — `data`/`issues` come from the Plan-A/B envelope):**
```ts
// ext → webview (models panel state)
export interface ModelsDataMessage {
  type: "modelsData";
  ok: boolean;                       // envelope.ok (false → show the error/issues, e.g. "update tan")
  models: unknown[];                 // envelope.data.models from `tan model list` (ModelListEntry[])
  toolchains: unknown[];             // envelope.data.toolchains from `tan model doctor`
  issues: { code: string; severity: string; message: string }[]; // envelope.issues (both calls merged)
}
// ext → webview (long build progress; mirrors SdkInstallProgressMessage)
export interface ModelBuildProgressMessage {
  type: "modelBuildProgress";
  log: string; done: boolean; success?: boolean;
}
// webview → ext (request a refresh, or build one/all)
export interface RequestModelsMessage { type: "requestModels"; }
export interface BuildModelMessage { type: "buildModel"; name?: string; } // name omitted = build all
```
(`models`/`toolchains` are `unknown[]` in the message and narrowed in the webview to the shapes Plan A defines: list entries `{name, source, compile, artifact:{exists,path,bytes,stale}}`; toolchains `{backend, tool, available, version, reason}`. Keeping them `unknown[]` at the boundary is the repo's convention for SDK-owned payloads — do not re-declare the SDK's schema here.)

- [ ] **Step 1 — add the three interfaces + union membership in `src/ideHub/messages.ts`.** Place near `HardwareExplorerDataMessage` (ext→webview) and `RequestSdkReleasesMessage` (webview→ext); add `ModelsDataMessage | ModelBuildProgressMessage` to `ExtToWebviewMessage` and `RequestModelsMessage | BuildModelMessage` to `WebviewToExtMessage`.
- [ ] **Step 2 — mirror all three in `packages/alp-webview/src/types.ts`** verbatim + add to the mirrored unions (`types.ts:496-508` ext side, `607-632` webview side).
- [ ] **Step 3 — verify: `pnpm run compile`** (portable-Node first). Expected: green (both `tsc --build` and the webview build compile the new union members).
- [ ] **Step 4 — commit.** `feat(models): message protocol for the Models panel (modelsData/modelBuildProgress/requestModels/buildModel)`.

---

### Task 2: Extension-side Models panel host + commands

`src/models/panel.ts` modeled on `HardwareExplorerPanel`: `refresh()` calls `tan model list` + `tan model doctor`, merges into `ModelsDataMessage`; `onMessage` handles `requestModels` (re-refresh) + `buildModel` (runs `tan model build`, streams progress). Register `alp.openModelsPanel` + `alp.buildModel`.

**Files:**
- Create: `src/models/panel.ts` (the panel host + a pure `toModelsData(listEnv, doctorEnv)` shaper) + `src/models/panel.test.js` target (via `test/`).
- Modify: `src/extension.ts` (import + `registerCommand("alp.openModelsPanel", …)` + `"alp.buildModel"`).
- Modify: `package.json` (`contributes.commands`: `alp.openModelsPanel` "Alp: Models", `alp.buildModel` "Alp: Build Model").
- Test: `test/models.panel.test.js` (node --test) for the pure shaper.

**Interfaces:**
- Consumes: `runAlpCommand(context, ["model","list"])`, `runAlpCommand(context, ["model","doctor"])`, `runAlpCommand(context, ["model","build", …])` (`src/alpCli/vscodeAdapter.ts:589`); `AlpEnvelope` (`src/alpCli/models.ts:8`); `buildWebviewHtml(webview, extensionUri, "models")`.
- Produces: a `ModelsDataMessage`; a `pure` function `toModelsData(listEnv: AlpEnvelope|null, doctorEnv: AlpEnvelope|null): ModelsDataMessage` extracted so it is unit-testable without VS Code/`tan`.

- [ ] **Step 1 — write the failing test** `test/models.panel.test.js`:
```js
const { test } = require("node:test");
const assert = require("node:assert");
const { toModelsData } = require("../out/models/panel.js");

test("toModelsData merges list + doctor envelopes", () => {
  const listEnv = { command:"model", ok:true, exitCode:0, project:{root:null,boardYaml:null},
    data:{ models:[{ name:"demo", source:"m.tflite", artifact:{ exists:true, stale:false } }] }, issues:[] };
  const doctorEnv = { command:"model", ok:true, exitCode:0, project:{root:null,boardYaml:null},
    data:{ toolchains:[{ backend:"cpu", tool:"", available:true }] }, issues:[] };
  const msg = toModelsData(listEnv, doctorEnv);
  assert.equal(msg.type, "modelsData");
  assert.equal(msg.ok, true);
  assert.equal(msg.models[0].name, "demo");
  assert.equal(msg.toolchains[0].backend, "cpu");
});

test("toModelsData surfaces a null/failed envelope as ok:false with an actionable issue", () => {
  const msg = toModelsData(null, null); // e.g. `tan` too old / no model command
  assert.equal(msg.ok, false);
  assert.ok(msg.issues.some((i) => /tan|model|update/i.test(i.message)));
  assert.deepEqual(msg.models, []);
});
```
- [ ] **Step 2 — run: fails** (`pnpm run compile && node --test test/models.panel.test.js`) — `toModelsData` undefined.
- [ ] **Step 3 — implement `src/models/panel.ts`.** `toModelsData`: when either envelope is `null` or `!ok`, return `{type:"modelsData", ok:false, models:[], toolchains:[], issues:[…merged issues, plus a synthesized "Update tan to a version with `tan model --format json` support." when the envelope is null]}`; else pull `listEnv.data.models` / `doctorEnv.data.toolchains` and merge `issues`. The `ModelsPanel` class mirrors `HardwareExplorerPanel` (singleton `currentPanel`, `createWebviewPanel`, `buildWebviewHtml(…, "models")`, `onDidReceiveMessage`). `refresh()` = `Promise.all([runAlpCommand(ctx,["model","list"]), runAlpCommand(ctx,["model","doctor"])])` → `post(toModelsData(a.outcome.envelope, b.outcome.envelope))`. `onMessage`: `requestModels` → `refresh()`; `buildModel` → run `tan model build` (with `name` → `["model","build","--model",name]` else `["model","build"]`) inside `vscode.window.withProgress`, teeing a `sendProgress`-style closure that posts `modelBuildProgress`, then `refresh()`.
- [ ] **Step 4 — register the commands** in `src/extension.ts` (`alp.openModelsPanel` → `showModelsPanel(context)`; `alp.buildModel` → open the panel then post a build) + `package.json` `contributes.commands`.
- [ ] **Step 5 — run: passes.** `pnpm run compile && node --test test/models.panel.test.js`.
- [ ] **Step 6 — commit.** `feat(models): extension panel host shelling tan model list/doctor/build`.

---

### Task 3: Webview Models feature (render list + doctor + build + inspector)

**Files:**
- Create: `packages/alp-webview/src/features/models/ModelsView.tsx`, `useModels.ts`, `ModelsView.module.css`, `index.ts`.
- Modify: `packages/alp-webview/src/App.tsx` — add `case "models": return <ModelsView />;` to `Router()`.

**Component contract (render from `ModelsDataMessage`; reuse tokens/CSS-modules like `hardware-explorer`):**
- `useModels()` — a `useReducer` + `window.addEventListener("message", …)` for `modelsData` and `modelBuildProgress`; posts `{type:"requestModels"}` on mount; exposes `{ ok, models, toolchains, issues, buildLog, building }` + `build(name?)` (`postMessage({type:"buildModel", name})`).
- `ModelsView` renders three sections:
  1. **Models** — one row per `models[]`: `name`, `source`, an artifact badge (`built` / `stale` / `missing` from `artifact.{exists,stale}`), a per-model **Build** button, and (on expand) an inspector calling — for v1 render the coverage from `tan model info` lazily OR fold coverage into the list payload later; v1 may show `artifact.bytes` + the build's `skipped`/`targets` after a build. Backend-coverage chips derive from the toolchain/doctor availability crossed with the SoM (reuse `acceleratorAvailability` if convenient, else show the doctor's `toolchains`).
  2. **NPU toolchains** (doctor) — one row per `toolchains[]`: `backend`, `tool`, an available/missing badge, `version` or the `reason` (guide text; **never a download link**).
  3. **Build progress** — when `building`, show `buildLog`; on `done`, refresh.
- An **"Edit models in Configurator"** link → `postMessage({type:"runCommand", command:"alp.openConfigurator"})` (reuse the existing card for editing; do NOT rebuild the editor).
- When `ok === false`, render the issues prominently (the "update tan" actionable message).

- [ ] **Step 1 — create the feature dir** (View + hook + css + index), modeled on `hardware-explorer/`.
- [ ] **Step 2 — wire `App.tsx` Router** `case "models"`.
- [ ] **Step 3 — verify: `pnpm run compile`** (the webview `vite build` compiles the new feature) green.
- [ ] **Step 4 — commit.** `feat(models): webview panel — models list, NPU toolchain doctor, build progress`.

---

### Task 4: LSP `models` field docs (thin-win)

`board.yaml` hover/completion currently doesn't document the `models` field.

**Files:**
- Modify: `src/lsp/service.ts` — add `models` to `FIELD_DOCS` (+ `CHILD_KEYS` for `models[].{name,source,compile}`), near the existing `inference` doc (`src/lsp/service.ts:96-122`).
- Test: `test/lsp.didOpen.test.js` (or the existing LSP test) — assert hover on `models:` returns the doc.

- [ ] **Step 1 — write the failing hover test** for `models:` (mirror the existing `inference` hover test).
- [ ] **Step 2 — run: fails.**
- [ ] **Step 3 — add the `models` `FIELD_DOCS` entry** (+ child keys) with a concise description (what `models[]` declares; that backends are SoM-derived, never customer-specified).
- [ ] **Step 4 — run: passes** (mind the ~13 baseline path-sep failures — only the new LSP test must go green + no NEW failures).
- [ ] **Step 5 — commit.** `feat(lsp): document the board.yaml models field on hover/completion`.

---

### Task 5: Launchers (Overview + Sidebar Hub)

**Files:**
- Modify: `packages/alp-webview/src/features/overview/OverviewView.tsx` — add a `PANELS` card that fires `runCommand("alp.openModelsPanel")`.
- Modify: `packages/alp-webview/src/features/sidebar-hub/SidebarHubView.tsx` — add an `ActionRow` → `alp.openModelsPanel`.

- [ ] **Step 1 — add the Overview card + Sidebar row** (copy an existing entry's shape; label "Models", a fitting codicon).
- [ ] **Step 2 — verify: `pnpm run compile`** green.
- [ ] **Step 3 — commit.** `feat(models): launch entries in Overview + Sidebar Hub`.

---

## Final gate (before the PR)

Portable-Node-24 on PATH, then from the repo root:
```bash
pnpm install --frozen-lockfile
pnpm run compile          # tsc --build + webview vp build — MUST be green
pnpm test                 # compile + node --test; new tests green + NO new failures beyond the ~13 path-sep baseline
```
Then the "verified-working" bar per the `alp-sdk-vscode` skill: F5 the Extension Development Host (or install the `.vsix`) and open the Models panel against a resolvable `tan`+SDK — **but** note that a *live* round-trip requires the envelope-wrapping `tan model` (Plan B) released; against an older `tan` the panel must degrade to the `ok:false` "update tan" message (Task 2 Step 3), not crash. Branch → PR to `main` (alp-sdk-vscode uses the PR-to-main flow; `dev` per its own protection — verify via `project_vscode_repo_merge_flow` memory); NO Claude/AI attribution.

## Self-review notes

- Implements spec §7 items 1–5 + 7 (panel, message types, commands, artifact/coverage inspector, toolchain doctor, LSP docs). Item 6 (deploy + run/observe UI) is **deferred** — Phase 2/3, gated on `tan model run` (not built) + the bench-gated Yocto runtimes.
- **Thin-extension held:** every model operation is a `runAlpCommand(["model", …])` shell of the envelope; no build/plan/model logic enters TypeScript. Editing reuses the Configurator card.
- **Honest degradation:** an older `tan` without the envelope model command yields `ok:false` + an actionable "update tan" issue, never a crash or an empty silent panel.
