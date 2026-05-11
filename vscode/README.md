# ALP SDK VS Code extension

First-class IDE support for ALP SDK projects -- `board.yaml`
schema-aware editing, a GUI configurator panel, per-OS dependency
bootstrap, and `west build / flash / run` wrappers.  Lives in the
`vscode/` subdirectory of the SDK so the schema, presets, and
loader the extension consumes always match the SDK revision
shipped alongside.

## What the extension gives you

- **Schema-aware `board.yaml`** -- attaches
  `metadata/schemas/board-config-v1.schema.json` to every file
  named `board.yaml` in the workspace.  You get autocomplete on
  SoM SKUs / carriers / libraries / log levels and red squigglies
  for schema violations, via the Red Hat YAML extension.
- **Snippets** -- type `alp-board-min`, `alp-carrier-populated`,
  `alp-inference`, `alp-iot`, or `alp-libraries` in any YAML file
  to expand a starter block.
- **GUI configurator** -- the *Alp: Open board configurator (GUI)*
  command opens a webview with dropdowns for the SoM SKU,
  carrier, OS, inference backend, and log level, plus checkboxes
  for every shipped library and every carrier-preset chip the
  EVK populates by default.  Reads + writes the workspace's
  `board.yaml` directly; the YAML editor remains available for
  power users.
- **Loader commands** -- one per emit mode, plus a one-keypress
  bundle:
  - *Alp: Generate alp.conf (zephyr-conf)*
  - *Alp: Generate alp.overlay (dts-overlay)*
  - *Alp: Generate CMake -D args*
  - *Alp: Generate Yocto local.conf snippet*
  - *Alp: Generate all* -- runs the four above in sequence and
    drops a single status-bar summary of which formats landed.
  Each writes its output under `build/generated/` and opens the
  result in a tab so the diff is obvious.
- **Validation** -- *Alp: Validate board.yaml* runs
  `scripts/validate_board_yaml.py` against the workspace file and
  reports schema / preset / hw_rev failures via a notification + the
  *ALP SDK* output channel.  The same validator runs automatically
  on every open / save of a `board.yaml` and its failures show up
  as inline diagnostics in the Problems panel under the source
  "alp-sdk" -- so a missing preset or an SDK / hw_rev mismatch
  surfaces before the build kicks off.
- **Dependency bootstrap** -- *Alp: Install dependencies (per-OS)*
  asks you whether you're targeting `zephyr`, `yocto`, or
  `baremetal`, then opens a terminal pre-populated with the right
  package-manager + `pip` commands for your host OS (Linux, macOS,
  Windows).  License-walled vendor toolchains (Alif / Renesas / NXP
  baremetal packs, Zephyr SDK installer) are surfaced as
  documentation pointers.
- **West wrappers** -- *Alp: West build / flash / Run under
  native_sim* delegate to `west` in a dedicated terminal so the
  output streams live and Ctrl-C / re-run works naturally.
- **Status bar** -- shows `<som> · <carrier> · <os>` parsed live
  from the workspace `board.yaml`; click it to open the
  configurator.

## Settings

| Setting                  | Default                       | What it picks                                        |
|--------------------------|-------------------------------|------------------------------------------------------|
| `alpSdk.path`            | (auto-detected)               | Path to the alp-sdk checkout (must contain `scripts/alp_project.py`). |
| `alpSdk.pythonPath`      | `python3` / `python`          | Interpreter for the loader + validator.              |
| `alpSdk.boardYamlPath`   | `board.yaml`                  | Workspace-relative path of the project's `board.yaml`. |
| `alpSdk.westCwd`         | (workspace root)              | Working directory for `west build/flash/run`.        |

## Building from source

```bash
cd vscode
npm install
npm run compile
npm run package    # produces alp-sdk-0.3.0.vsix
```

Install the resulting `.vsix` via *Extensions: Install from
VSIX...*.  Once the marketplace publisher is configured, releases
ship via `vsce publish`.

## Architecture

The extension is intentionally thin -- the GUI configurator,
snippets, and schema attachment are the only IDE-side concerns;
everything else delegates to the SDK's existing `scripts/`:

- *Generate* commands invoke `scripts/alp_project.py`.
- *Validate* invokes `scripts/validate_board_yaml.py`.
- *West* commands delegate to the user's `west` install in a
  terminal so the SDK doesn't shadow Zephyr's build system.

The configurator panel reads released SoM SKUs by listing
`metadata/e1m_modules/<MPN>/som.yaml`, released carriers from
`metadata/carriers/<name>/board.yaml`, and library + enum choices
from `metadata/schemas/board-config-v1.schema.json` -- adding a
new MPN or carrier in the SDK lights up the dropdown
automatically with no extension change.
