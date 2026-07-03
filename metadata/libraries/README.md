# Curated third-party library manifests (`metadata/libraries/`)

One file per curated third-party library, `metadata/libraries/<name>.yaml`.
This directory is the **single source of truth** for the ADR 0018 library
layer: a project pulls a library in with one declaration in its `board.yaml`,

```yaml
libraries: [lvgl, cmsis-dsp, nanopb]
```

and the orchestrator emits the per-OS wiring from the manifest through the
ordinary `--emit` contract (ADR 0014). Nothing is fetched or forked: west
(Zephyr) and OpenEmbedded/meta-alp-sdk (Yocto) own the actual upstream pin —
ADR 0017 applies to libraries verbatim.

Not to be confused with [`metadata/library-profiles/`](../library-profiles/),
which carries compile-time config headers + HW-accelerator bindings for the
closed per-core `cores.<id>.libraries:` enum. This directory is the
manifest-driven **top-level** `libraries:` selection.

## The curated set

| Library     | Tier | Version    | Licence    | OSes      | Needs |
|-------------|------|------------|------------|-----------|-------|
| `lvgl`      | A    | 9.5.0      | MIT        | zephyr, yocto | ≥64 KiB RAM |
| `cmsis-dsp` | A    | (west pin) | Apache-2.0 | zephyr    | — |
| `cmsis-nn`  | A    | (west pin) | Apache-2.0 | zephyr    | Cortex-M |
| `nanopb`    | A    | 0.4.9.1    | Zlib       | zephyr    | — |
| `zcbor`     | A    | 0.9.1      | Apache-2.0 | zephyr    | — |

`alp doctor` reports the selection for the project in scope (tier + licence +
compatibility), reading these same manifests — so the CLI and alp-studio's
library picker can never disagree.

## Manifest shape

See [`../schemas/library-v1.schema.json`](../schemas/library-v1.schema.json)
for the authoritative schema. A manifest declares:

```yaml
schema_version: 1
name: lvgl                     # must match the filename (<name>.yaml)
description: "..."             # one-liner, surfaced in alp doctor
tier: A                        # A (curated, CI-built) | B (recipe-only)
version: "9.5.0"               # the pinned upstream version (never a range)
license: MIT                   # SPDX id from the allowlist (below)

requires:                      # all fields optional; omitted == no requirement
  capabilities: [gpu2d]        # each key MUST be a real SoC capability
  min_flash_kib: 0             # checked against soc_flash_mb * 1024
  min_ram_kib: 64              # checked against soc_ram_kb
  core_class: m                # m (Cortex-M) | a (Cortex-A) -- at least one such core
  os: [zephyr]                 # at least one core runs one of these

integration:                   # at least one OS section required
  zephyr:                      # emitted into the slice's alp.conf
    module: lvgl               # west module name (documentation; west owns the pin)
    kconfig: [CONFIG_LVGL=y]   # upstream Kconfig symbols -- transcribed, never invented
  yocto:                       # emitted into the slice's local.conf
    image_install: [lvgl]      # appended to IMAGE_INSTALL
  baremetal:                   # emitted into the slice's cmake args
    cmake: "find_package(...)"
```

### `requires:` — keep it minimal and honest

Only constrain what upstream genuinely requires, and cite the source in a
comment. Notes from the initial set:

- There is **no `display` capability** in the alp-sdk capability layer — a
  display can be an SPI SSD1306 (chip driver) or a MIPI-DSI panel, with no
  single capability that unifies them. LVGL is display-agnostic (it flushes
  pixels through a callback), so it gates on a RAM floor, not a capability.
- `requires.capabilities` keys are cross-checked by
  `scripts/validate_metadata.py` against the SoC capability vocabulary
  (`soc-spec-v1.schema.json` → `capabilities`). An unknown key is rejected.
- Grounding matters: every Kconfig symbol, module name, and version must be
  transcribed from the pinned Zephyr v4.4.0 workspace / OE recipe (record the
  file and `west.yml` revision in a manifest comment), or omitted. Never
  invent one.

## Licence allowlist

The `license:` field is validated against a permissive allowlist so a
copyleft or proprietary surprise cannot ride in through a `libraries:`
selection:

```
Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, Zlib, MIT-0
```

**Extending this allowlist is a deliberate human decision, not a metadata
edit.** Add the SPDX id in BOTH `schemas/library-v1.schema.json`
(`license.enum`) and this list, in the same change, with the legal rationale
in the commit body. GPL-family and proprietary licences are rejected by
design.

## Adding a library

1. Ground the facts from the pinned workspace: `source ~/alp-env.sh`, then
   `grep` the module's `Kconfig` for the enable symbol, read the version file,
   confirm the SPDX id from the module's `LICENSE`, and read the `west.yml`
   `revision:`. Record these in the manifest as comments.
2. Write `metadata/libraries/<name>.yaml` per the shape above. Keep `requires:`
   minimal and honest.
3. `python3 scripts/validate_metadata.py` — schema + capability-key + licence
   + filename checks must pass.
4. Add an emitter test to `tests/scripts/test_library_layer.py` proving the
   Kconfig (or `IMAGE_INSTALL`) lands and any `requires:` constraint is
   enforced.
5. Choose the tier honestly.

### The Tier A promotion bar

A library ships as **Tier A** only if it clears all of:

- version-pinned upstream (no floating revision);
- builds in alp-sdk CI for at least one board per supported family;
- ships one teaching example under `examples/`;
- breakage blocks release.

Anything that can't yet meet that bar ships as **Tier B** (recipe-only): the
wiring + compatibility metadata are maintained and emitted, but the library is
not built in alp-sdk CI, and `alp doctor` labels it. Promotion B → A requires
a dedicated owner and a CI build lane.
