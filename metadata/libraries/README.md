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
| `modbus`    | A    | 4.4.0      | Apache-2.0 | zephyr    | Cortex-M; Zephyr in-tree subsys |
| `micro-ros` | B    | humble     | Apache-2.0 | zephyr    | Cortex-M; west pin + CONFIG_MICROROS ‡ |
| `ros2`      | B    | humble     | Apache-2.0 | yocto     | Cortex-A; meta-ros2-humble layer ‡ |
| `lwm2m`     | B    | 4.4.0      | Apache-2.0 | zephyr    | Zephyr (in-tree subsys) |
| `coap`      | B    | 4.4.0      | Apache-2.0 | zephyr    | Zephyr (in-tree subsys) |
| `aws-iot`   | B    | v3.1.5     | Apache-2.0 | zephyr    | Zephyr; west project pin § |
| `azure-iot` | B    | 1.5.0      | MIT        | zephyr    | Zephyr; west project pin § |
| `canopennode` | B  | dec12fa3f0d790cafa8414a4c2930ea71ab72ffd | Apache-2.0 | zephyr | Cortex-M; optional west pin; CAN controller |
| `micropython` | B  | v1.24.1    | MIT        | zephyr    | Cortex-M; source pin; dedicated owner needed |

`alp doctor` reports the selection for the project in scope (tier + licence +
compatibility), reading these same manifests — so the CLI and alp-studio's
library picker can never disagree.

**‡ The ADR 0018 flagship — micro-ROS + ROS 2 (ADR 0010 heterogeneous proof).**
One project runs a micro-ROS node on the Cortex-M / Zephyr peer and ROS 2 on
the Cortex-A / Yocto peer. Two honest caveats are recorded in the manifest
headers rather than papered over:

- `micro-ros` is pinned in `west.yml` from the upstream
  `micro_ros_zephyr_module` Humble branch at
  `cfbddc5e4334317a1036e883ce8f6af12b1da66a`. Its Zephyr integration names the
  west module and transcribes the real master symbol from
  `modules/libmicroros/Kconfig`: `CONFIG_MICROROS=y`.
- `ros2` is **Tier B (recipe-only)**: its wiring is grounded in-tree
  (`rclcpp` in `meta-alp-sdk/recipes-images/alp-image-common.inc`,
  `meta-ros2-humble` as a `LAYERRECOMMENDS`), but alp-sdk CI does not build it.
  A build must add `meta-ros2-humble` to `bblayers.conf` (named follow-up).
- The **cross-core RMW bridge** (micro-ROS↔ROS 2 over UDP-on-virtio or a custom
  RMW over the existing RPMsg transport, ADR 0016) is **bench-gated** and out of
  scope here — likely its own ADR.

**The ADR 0018 cloud / connectivity Tier-B group.** Four manifests deliver the
device-to-cloud story; all Tier B (recipe-only), split by grounding:

- `lwm2m` / `coap` are **upstream Zephyr in-tree subsystems** — real enable
  symbols transcribed from the pinned v4.4.0 tree (`CONFIG_LWM2M`,
  `subsys/net/lib/lwm2m/Kconfig`; `CONFIG_COAP`, `subsys/net/lib/coap/Kconfig`),
  version `4.4.0` (the Zephyr release the subsystem ships with),
  `integration.zephyr.module: null` (no separate west module — the `zephyr`
  project carries them). LwM2M `select COAP`, so `libraries: [lwm2m]` pulls its
  transport in. LwM2M could merit Tier A on strength, but the ADR groups
  connectivity/cloud as B and there is no CI build lane + example yet
  (promotion is a named follow-up).
- **§ `aws-iot` / `azure-iot` are pinned source manifests** — verified **not
  imported by Zephyr's own west manifest** (`west list` has no aws/azure/iot
  entry). Unlike the micro-ROS flagship, neither generic CMake C SDK has an
  official upstream **Zephyr module.yml** or master Kconfig symbol, so
  Zephyr-side packaging/build glue remains a follow-up. The source pins are now
  exact and reproducible: AWS `aws-iot-device-sdk-embedded-C` at `v3.1.5`
  (Apache-2.0) and Azure `azure-sdk-for-c` at `1.5.0` (MIT). Each declares an
  **enable-by-presence** Zephyr section (`module:` naming the real upstream repo,
  `west:` carrying the exact project pin, **no** `kconfig:`) — no enable symbol
  is invented; emit renders the selection tag and `--emit west-libraries` emits
  concrete west project entries, with no `CONFIG_` line until packaging confirms
  a real symbol.

**The ADR 0018 industrial / scripting additions.** Three manifests close the
remaining curation set without inventing capabilities or symbols:

- `modbus` is a **Tier A Zephyr in-tree subsystem** (`CONFIG_MODBUS` from
  `subsys/modbus/Kconfig`, version `4.4.0`, Apache-2.0).  The native_sim CI
  lane uses `CONFIG_MODBUS_RAW_ADU`; serial RTU/ASCII deployments need a UART
  plus a devicetree `zephyr,modbus-serial` node.
- `canopennode` is **Tier B CAN**: Zephyr pins it as an optional west project
  in `submanifests/optional.yaml` and its integration glue exposes
  `CONFIG_CANOPENNODE`.  It needs a real Zephyr CAN controller, but alp-sdk has
  no CAN capability key, so the manifest records only OS/core constraints.
- `micropython` is **Tier B scripting**: the pinned workspace has no
  `modules/lib/micropython` checkout, so the manifest is an enable-by-presence
  source pin to upstream `micropython/micropython` at `v1.24.1` with no
  invented Kconfig.  Tier-A promotion needs a dedicated owner (ADR 0018).

**Memfault — deliberately NOT shipped.** Memfault's `memfault-firmware-sdk` is
not pinned in `west.yml`, and its licence is the proprietary **Memfault SDK
License** (source-available, use-with-Memfault-services), which is **not** in the
permissive allowlist below (Apache-2.0/MIT/BSD-2/BSD-3/Zlib/MIT-0). Per the
ADR 0018 non-goal, a copyleft/proprietary licence must not ride in through a
`libraries:` selection, and forcing a wrong SPDX id to pass validation is
forbidden. It can only be added if a human legal review extends the allowlist
(schema `license.enum` + this list, same change) with the Memfault licence — a
deliberate decision, not a metadata edit.

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
    west:                      # optional exact west project pin when Zephyr does not import it
      name: aws-iot-device-sdk-embedded-C
      url: https://github.com/aws/aws-iot-device-sdk-embedded-C.git
      revision: v3.1.5         # exact tag/SHA, never main/master
      path: modules/lib/aws-iot-device-sdk-embedded-C
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
- Grounding matters: every Kconfig symbol, module name, west project pin, and version must be
  transcribed from the pinned Zephyr v4.4.0 workspace / OE recipe (record the
  file and `west.yml` revision in a manifest comment), or an exact upstream
  release tag/SHA for repos Zephyr does not import. Never
  invent one.
- A Zephyr integration section may name its west `module:` **without** a
  `kconfig:` list (`integration.zephyr` requires only that one of the two is
  present). This is the honest shape for an *enable-by-presence* module — one
  that builds when it is in the workspace with no master `CONFIG_*=y` switch —
  or a curated module whose west pin is a documented prerequisite not yet in
  this checkout's `west.yml` (for example, AWS/Azure SDK packaging follow-ups).
  Fabricating a Kconfig to
  fill the slot is exactly what this relaxation exists to prevent: name the
  module, document the prerequisite in the header, and transcribe the symbol
  only once it is real.

## Licence allowlist

The `license:` field is validated against a permissive allowlist so a
copyleft or proprietary surprise cannot ride in through a `libraries:`
selection:

```
Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, Zlib, MIT-0, BSL-1.0, CC0-1.0
```

**Extending this allowlist is a deliberate human decision, not a metadata
edit.** Add the SPDX id in BOTH `schemas/library-v1.schema.json`
(`license.enum`) and this list, in the same change, with the legal rationale
in the commit body. GPL-family and proprietary licences are rejected by
design. `BSL-1.0` (Boost, `catch2`) and `CC0-1.0` (public-domain-equivalent,
`minimp3`) were added per the #610 WS6-c maintainer legal-review decision;
the same decision dropped `tinygsm` (LGPL-3.0) and `libhelix` (RPSL-1.0)
from the curated set rather than admit their copyleft/non-permissive
licences into this allowlist.

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
