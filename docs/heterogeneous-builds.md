# Heterogeneous builds — Zephyr + Yocto on the same SoM

This guide walks through writing your first **dual-app project** for
E1M-V2N101: Yocto Linux on the four Cortex-A55 cores plus Zephyr on
the Cortex-M33 system-manager, the two halves talking over RPMsg.
You'll declare both halves in a single `board.yaml`, let
`west alp-build` fan out into per-core slices, and end up with a
flashable bundle that covers Linux + Zephyr + the on-module GD32
helper MCU.

The same pattern generalises to **E1M-AEN801** (A32 + M55-HP + M55-HE),
**E1M-NX9101** (A55 + M33), and any future heterogeneous SoM.

> If you're targeting a single-OS SoM (e.g. AEN E3/E4 with M55 cores
> only), follow [`docs/firmware-quickstart.md`](firmware-quickstart.md)
> instead.  The orchestrator handles single-slice fan-outs too, but
> you don't need the cross-core machinery this guide focuses on.

## 1. What this guide covers

By the end you'll have:

- A V2N project that boots Yocto Linux on the A55 cluster.
- A Zephyr image running on the M33-SM that the kernel brings up via
  remoteproc on first boot.
- A two-way RPMsg channel between the two halves, accessed through
  `<alp/rpc.h>`.
- A `system-manifest.yaml` that feeds `west alp-image`,
  `west alp-flash`, and OTA.

Out of scope: writing Yocto recipes from scratch (Yocto docs);
writing Zephyr drivers from scratch (Zephyr docs); the wire-level
RPMsg protocol details (OpenAMP docs).

## 2. Prerequisites

1. West workspace bootstrapped — `bash scripts/bootstrap.sh` from the
   SDK root.  See [`docs/getting-started.md`](getting-started.md) §1–3.
2. Zephyr SDK installed (1.0.1, `ZEPHYR_SDK_INSTALL_DIR` exported) for
   the Zephyr slice's real-silicon target.  Not required for
   `native_sim/native/64` smoke builds — those use host gcc with
   `ZEPHYR_TOOLCHAIN_VARIANT=host`.
3. Yocto build host set up (50+ GB free, Poky host packages).  See
   [`docs/getting-started.md`](getting-started.md) "Yocto host
   requirements" — the orchestrator delegates to bitbake; it does not
   ship a Yocto bootstrap.
4. Plan for ~30 GB of `build/<core>-yocto/tmp/` on the first cold
   build.  Subsequent builds reuse `sstate-cache` and stay small.

## 3. Project layout

A multi-image project keeps each per-core image in its own
sub-directory.  **The canonical layout names each app folder after its
core ID** (`m33_sm/`, `a55_cluster/`) — that keeps the project
self-describing and lets an IDE map cores ↔ folders by convention.
What actually binds a core to its folder is the single source, the
`cores.<id>.app:` path in `board.yaml`; the folder name is convention,
not magic, so you *may* alias one when a name reads better (this
example uses `linux/` for the a55_cluster's Yocto side).

```
examples/multicore/rpmsg-v2n/
├── board.yaml                       (declares a55_cluster + m33_sm)
├── README.md
├── linux/                           (a55_cluster's app — aliased via app: ./linux)
│   ├── CMakeLists.txt
│   └── src/main.c                   (consumer using <alp/rpc.h>)
└── m33_sm/                          (m33_sm's app — folder == core ID, the canonical convention)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c                   (producer using <alp/rpc.h>)
```

The `cores.<id>.app:` path is the binding; `m33_sm/` matches its core
ID (canonical), while `linux/` is an explicit alias set by
`app: ./linux`.  Omitting `app:` entirely is also valid — the core
then builds the SoM's stock default app (`alp-image-edge` on a Linux
core, `alp-stock-shim` on a Zephyr core).  Both defaults are
buildable: `alp-image-edge` for Linux-class cores, and the minimal
SDK-owned `firmware/alp-stock-shim/` idle image for Zephyr peer
cores.  Override `cores.<id>.app` with a real app when that peer core
should run project firmware instead of the stock idle image.

Single-OS examples don't change shape: they keep their flat `src/`
layout and declare a single core in `board.yaml`.  The sub-directory
split is opt-in per project.

## 4. The `cores:` block, walked through

Here's a complete V2N `board.yaml` v2:

```yaml
som:
  sku: E1M-V2N101
  hw_rev: r1

preset: e1m-x-evk
cores:
  a55_cluster:
    app: ./linux         # os: omitted -- A-cores default to yocto per SoM topology
    image: alp-image-edge
    peripherals: [ethernet, usb, emmc]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    app: ./m33_sm        # os: omitted -- M-cores default to zephyr per SoM topology
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
```

**`os`** — runtime for this core.  **Optional** — every core has a
natural runtime baked into the SoM preset's `topology:` block
(Cortex-M → Zephyr, Cortex-A → Yocto Linux).  Write `os:` only to
**override** the topology default:

| Value      | When to write it                                                              |
|------------|-------------------------------------------------------------------------------|
| `off`      | Intentionally skip a core (e.g. AEN's A32 cluster, M55-HE peer).              |
| `baremetal`| Rare — hand-written firmware on a core that would normally run Zephyr.        |
| `zephyr` / `yocto` | Almost never needed — these match the topology defaults.              |

`off` is a first-class state.  If your firmware doesn't need the
M33-SM, write `os: off` and the orchestrator skips it — no implicit
"did we forget a core?" failure mode.

**`app`** — relative path (from the project root) to the source tree
for this slice.  Zephyr needs `prj.conf` + `CMakeLists.txt`.  Yocto
takes a CMake project (built as a recipe) or omits this entirely when
`image:` overrides the stock recipe.  Baremetal takes a CMake project.

**`image`** — Yocto-only.  Names the bitbake image recipe; defaults
to `alp-image-edge` from `meta-alp-sdk`.  Override for a custom rootfs.

**`peripherals`** — per-core peripheral classes.  On Yocto controls
which kernel drivers ship; on Zephyr gates `CONFIG_ALP_SDK_PERIPH_*`
symbols.  Pay code-size only for what you use.

**`libraries`** — per-core libraries to link in.  Names resolve via
`metadata/libraries/`.

**`iot`** — per-core IoT capabilities (Wi-Fi, BLE, MQTT, HTTPS).
Typically only on the A-cluster on V2N — the M33-SM doesn't carry
networking here.

**`inference`** — per-core inference tuning.  Carries only
`default_arena_kib:` (per-model memory budget).  The dispatcher
set (which NPUs compile in) is silicon-determined by the SoM
preset's `capabilities:` block — the SDK compiles in **every**
NPU the SoM has, plus the TFLM CPU fallback.  Apps pick which one
to run per-handle at runtime via `alp_inference_open(.backend=…)`,
which is what makes concurrent multi-NPU dispatch possible on
V2M101 (DRP-AI3 + DEEPX DX-M1 running independent models at the
same time).  See `docs/tutorials/16-inference-mobilenet.md` for
the full pattern.

### Migrating from v1

- Top-level `os:` is **removed**.  Replace with one `cores:` entry
  per programmable core.
- Top-level `peripherals:` / `libraries:` / `inference:` / `iot:`
  move **per-core** under `cores.<id>`.
- `board.populated:` + `chips:` and `diagnostics:` are
  **unchanged** — they describe physical assembly + project-wide
  diagnostics respectively.

## 5. The `ipc:` block

Each entry declares one cross-core channel.

```yaml
ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg
```

- **`kind: rpmsg`** — the only supported value as of v0.6.  Future
  kinds (raw shmem, virtio-net) are reserved.
- **`endpoints`** — the cores sharing this channel.  Both must have
  `os: != off`.  Exactly two; RPMsg is point-to-point today.
- **`carve_out_kb`** — shared-memory region size in kibibytes.  The
  orchestrator allocates it from the auto-derived region table
  (from `metadata/socs/.../<part>.json variants[].sram_banks_kb` +
  `mram_mb`) or from the explicit `memory_map:` override block if
  the SoM preset defines one for non-stock partitioning.  Prefers
  non-cacheable regions on SoMs with no M-class cache
  (V2N) and cacheable regions with auto-generated cache-maintenance
  hooks on SoMs that do (AEN).
- **`name`** — stable identifier.  Becomes the resource-table label
  on OpenAMP, the Linux DT `reserved-memory` node label, and the
  `#define` prefix in the generated header.  Stick to
  `[a-z][a-z0-9_]+`.

For each `ipc:` entry, `west alp-build` emits a header both halves
`#include`:

For a channel named `alp_default_rpmsg`, the stem is its
upper-cased name, so the generated `#define`s are
`ALP_IPC_<STEM>_NAME` / `_ADDR` / `_SIZE` / `_SRC_EPT` /
`_DST_EPT` / `_MBOX_CH`:

```c
/* build/generated/alp/system_ipc.h — auto-generated, do not edit */
#define ALP_IPC_<STEM>_NAME       "alp_default_rpmsg"
#define ALP_IPC_<STEM>_ADDR       0x00010000u
#define ALP_IPC_<STEM>_SIZE       0x00080000u
#define ALP_IPC_<STEM>_SRC_EPT    0x000004e6u
#define ALP_IPC_<STEM>_DST_EPT    0x000004e7u
#define ALP_IPC_<STEM>_MBOX_CH    0u
```

The macro stem is the channel `name:` upper-cased (so
`alp_default_rpmsg` → `ALP_IPC_ALP_DEFAULT_RPMSG_*`).  Both
`linux/src/main.c` and
`m33_sm/src/main.c` `#include <alp/system_ipc.h>` and use the same
constants.  Endpoint IDs are derived from `name` deterministically via
FNV-1a — re-running the build produces byte-identical headers.  Drift
between the Linux DT and the Zephyr overlay becomes impossible.

**Blocked carve-outs.**  When a SoM preset still carries TBD
`mailbox.controller` or TBD `memory_map.base` / `size` (the common
case while the SoM is being HW-mapped), the orchestrator emits the
manifest entry as `status: blocked` + `reason: ...` instead of
aborting.  The generated `<alp/system_ipc.h>` carries an `#error`
directive for the blocked channel so the slice build trips at compile
time — but `--emit system-manifest` succeeds, which keeps the CI
manifest-shape gate green while the preset metadata is in flight.
Fill in the missing values in `metadata/e1m_modules/<sku>.yaml` to
unblock.

## 6. Building

```bash
west alp-build examples/multicore/rpmsg-v2n
```

The orchestrator:

1. Loads + validates `board.yaml` against the board.yaml schema.
2. Resolves the SoM preset → topology defaults → effective per-core
   mapping.
3. For each core with `os: != off`, materialises per-core config
   (`build/m33_sm-zephyr/alp.conf`,
   `build/a55_cluster-yocto/conf/local.conf`).
4. Emits shared generated artefacts (`generated/alp/system_ipc.h`,
   `generated/dts-reservations.dtsi`).
5. Registers helper-MCU artefacts (GD32, CC3501E).
6. Dispatches slice builds in parallel.
7. Writes `build/system-manifest.yaml` joining everything together.

Output layout:

```
build/
├── a55_cluster-yocto/
│   ├── conf/local.conf
│   └── tmp/deploy/images/e1m-v2n101-a55/{rootfs.wic.gz, Image, *.dtb}
├── m33_sm-zephyr/
│   └── zephyr/zephyr.elf
├── helper-gd32/
│   └── gd32_bridge.bin
├── helper-cc3501e/
│   └── cc3501e_otp.blob
├── generated/
│   ├── alp/system_ipc.h           (matches the `<alp/system_ipc.h>` include)
│   ├── dts-reservations.dtsi
│   └── alp_hw_info_build.h
└── system-manifest.yaml
```

The `generated/` directory ends up on each slice's include path, so
`#include <alp/system_ipc.h>` resolves from either the Yocto recipe
or the Zephyr CMakeLists.

**Manifest determinism.**  `system-manifest.yaml` is byte-stable
across rebuilds — re-running `west alp-build` after `west alp-clean`
yields an identical manifest, which `pr-alp-build.yml` enforces.
Wall-clock fields (per-slice `duration_s`) live on the runtime Slice
dataclass but never land in the manifest; the cache state in
`build/.alp-build-state.json` is internal and not part of the
declarative output either.

**Manifest contract (IDE / tooling).**  `system-manifest.yaml` is the
single derived projection of `board.yaml` — one `slices[]` entry per
per-core image (its `os`, `build_dir`, `output_artefact`,
`board`/`machine`, and `flash_method`/`flash_args`), plus the `ipc:`
links and `helper_mcus:`.  Tools — the alp-sdk-vscode extension, CI,
the flasher — read **this** to manage a multi-image project instead of
re-deriving folder layout and build wiring from `board.yaml` + the SoM
presets.  Its shape is pinned by
[`metadata/schemas/system-manifest-v1.schema.json`](../metadata/schemas/system-manifest-v1.schema.json);
`scripts/check_system_manifest.py` validates the orchestrator's output
against it so the emitter and the contract move in lockstep.  Validate
a real build's manifest with:

```bash
python3 scripts/check_system_manifest.py --manifest build/system-manifest.yaml
```

**Stability policy.**  `schema_version: 1` is **additive-only** — new
*optional* fields may appear in any tagged release, but nothing existing
is renamed, retyped, made required, or removed.  A breaking change ships
as `schema_version: 2` through a deprecation cycle: the replaced shape is
emitted alongside its replacement for at least one tagged release before
removal.  Consumers should tolerate unknown fields so an additive v1
change is a no-op for them.

This pairs with the build plan below: the **manifest** is the *result*
(what was/your-to-be built, per image) that an IDE reads to drive
build/run/debug/flash; the **build plan** is the *write-free recipe* to
drive the build itself.

**Machine-readable build plan.**  Tooling that wants to drive the
build itself — the `alp` CLI / IDE extension does — consumes the plan
instead of re-deriving it:

```bash
PYTHONPATH=scripts python3 -m alp_orchestrate --input board.yaml --emit build-plan
```

The JSON carries one entry per non-`off` core (build dir, the resolved
app source dir, the exact tool command, env) plus every generated
artefact **with its contents**, so a consumer materialises files and
runs commands without any planner logic of its own.  Every relative
path resolves against the input `board.yaml`'s own directory, never the
CLI's CWD, so the plan is deterministic, write-free, and versioned by
its own `schemaVersion` — see
[ADR 0014](adr/0014-build-plan-emit-cli-contract.md) for the contract.

Its shape is pinned by
[`metadata/schemas/build-plan-v1.schema.json`](../metadata/schemas/build-plan-v1.schema.json);
`scripts/check_build_plan.py` validates the emitter's output against it,
the same emitter-and-contract lockstep `check_system_manifest.py`
enforces for the manifest above.  Validate a real plan with:

```bash
python3 scripts/check_build_plan.py --plan build-plan.json
```

### Build receipts (`build-receipt-v1`)

A **build receipt** is deterministic provenance for a release build: given
the same board.yaml, build-plan, and produced images, `scripts/build_receipt.py`
composes the same receipt byte-for-byte — no wall-clock timestamp, and every
path is stored repo-relative so the receipt doesn't change just because it
was built from a different checkout location. It's a pure composer over
inputs that already exist (the build-plan, `board.yaml`, and each core's
output image), not a new build step.

The top-level fields:

- `source` — the SDK's git revision and whether the tree was dirty.
- `config` — the resolved `boardYaml` path + its digest, the `sku`, and the
  build-plan's digest (plus the lockfile digest, if supplied).
- `toolchain` — the toolchain identity recorded by the build-plan.
- `images` — one entry per core: its build path, sha256, and size in bytes.
- `provenance` — placeholders (`sbomRef`, `attestationRef`) for later slices.

Its shape is pinned by
[`metadata/schemas/build-receipt-v1.schema.json`](../metadata/schemas/build-receipt-v1.schema.json);
`scripts/check_build_receipt.py` validates that schema stays closed and
well-formed. Wiring a receipt into `release.yml` and populating the SBOM /
attestation refs are later #610 §7 slices — this slice only pins the shape
and the composer.

### Iterating on one slice

The Yocto cold build takes hours; the Zephyr build takes seconds.
When you're iterating on the M-side firmware, rebuild only that slice:

```bash
west alp-build examples/multicore/rpmsg-v2n --core m33_sm
```

The orchestrator skips the Yocto fan-out, re-uses the previous
manifest, and rebuilds only `build/m33_sm-zephyr/`.  Slice failures
don't cascade — `system-manifest.yaml` carries per-slice
`status: ok | failed`; re-running re-attempts only the failed slices.

## 7. Flashing

```bash
west alp-image     # → build/image-bundle/alp-system.zip + .swu (Mender)
west alp-flash     # programs attached hardware
```

`alp-image` consumes `system-manifest.yaml` and assembles a single
flashable bundle:

- The Yocto `.wic.gz` rootfs.
- The Zephyr `.elf` (installed into the rootfs at
  `/lib/firmware/alp/E1M-V2N101/m33_sm.elf` so remoteproc picks it up
  on first boot).
- Helper-MCU firmware (`gd32_bridge.bin`, `cc3501e_otp.blob`) —
  bundled automatically.
- A Mender `.swu` for OTA.

`alp-flash` walks the manifest's `boot_order:` and programs each piece
with the right backend tool (vendor flasher for the SoC,
openocd-via-SWD for the GD32 helper, USB-CDC bootloader for CC3501E).
You don't need to remember which tool covers which piece.

## 8. Debugging

### Per-slice logs

Each slice gets its own log directory under `build/<core>-<os>/`:

- `build/m33_sm-zephyr/build.log` — Zephyr CMake + ninja output.
- `build/a55_cluster-yocto/log/bitbake.log` — bitbake task output.
- `build/helper-gd32/build.log` — GD32 firmware build.

`system-manifest.yaml` carries each slice's `log_path:` so tooling
jumps straight to the right log on a failure.

### Attaching a debugger

- **A55 cluster (Linux):** `alp-image-edge` ships with `gdbserver`.
  SSH in, attach to your process.
- **M33-SM (Zephyr):** SWD via openocd or J-Link.  The orchestrator
  installs `build/m33_sm-zephyr/openocd.cfg`; `west debug --build-dir
  build/m33_sm-zephyr` attaches a GDB session.
- **Cross-core sanity check:** print your endpoint IDs on both sides
  with `printk("ept=%u\n", ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT)` — they
  should match the manifest's `ipc[].rpmsg_endpoint_ids` field.

### Renode smoke test

You don't need a board to verify the heterogeneous handshake:

```bash
west alp-renode
```

Renode loads both slice images, simulates RPMsg over its mailbox
peripheral, and runs a name-service ping/pong.  CI uses the same
command in `pr-renode-dual-os.yml`.

## 9. Cross-core API

`<alp/rpc.h>` is the customer-facing IPC API.  It sits on OpenAMP and
uses the generated endpoint constants — apps don't type addresses,
endpoint IDs, or mailbox channels by hand.

### Producer (M33-SM)

```c
/* m33_sm/src/main.c */
#include <alp/rpc.h>
#include <alp/system_ipc.h>      /* generated by west alp-build */
#include <zephyr/kernel.h>

int main(void) {
    alp_rpc_channel_t *ch = alp_rpc_open(&(alp_rpc_config_t){
        .name      = ALP_IPC_ALP_DEFAULT_RPMSG_NAME,
        .src_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT,
        .dst_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT,
        .mbox_ch   = ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH,
    });
    if (ch == NULL) {
        return -1;  /* alp_last_error() reports why */
    }

    while (1) {
        float temperature_c = read_thermistor();
        alp_rpc_call(ch, "temperature",
                     &temperature_c, sizeof(temperature_c));
        k_msleep(1000);
    }
}
```

### Consumer (A55)

```c
/* linux/src/main.c */
#include <alp/rpc.h>
#include <alp/system_ipc.h>      /* generated by west alp-build */
#include <stdio.h>
#include <unistd.h>

static void on_temperature(const void *buf, size_t len, void *user) {
    if (len == sizeof(float)) {
        printf("[a55] temperature=%.2f C\n", *(const float *)buf);
    }
}

int main(void) {
    alp_rpc_channel_t *ch = alp_rpc_open(&(alp_rpc_config_t){
        .name      = ALP_IPC_ALP_DEFAULT_RPMSG_NAME,
        .src_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT,  /* swap src/dst */
        .dst_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT,
        .mbox_ch   = ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH,
    });

    alp_rpc_subscribe(ch, "temperature", on_temperature, NULL);

    for (;;) pause();
}
```

Both sides `#include` the same generated header, so endpoint IDs match
by construction.  The producer's `src_ept` is the consumer's `dst_ept`
and vice versa — that symmetry is the only piece a developer keeps
straight.  `mbox_ch` is the same on both sides.  For multiple
channels, declare multiple `ipc:` entries with distinct `name:` values.

## 10. Common pitfalls

**Forgetting to declare `ipc:`.**  Call `alp_rpc_open()` for a name
that doesn't appear in any `ipc:` block and you won't compile —
`<alp/system_ipc.h>` doesn't carry the matching constants, so the
`ALP_IPC_<name>_NAME` macro is undeclared at the call site.  Every cross-core
touchpoint is declared at build time, not discovered at runtime.  If
you hardcode the strings instead, the runtime returns
`ALP_ERR_NOSUPPORT` because no carve-out backs the name; check
`alp_last_error()`.

**Cache coherency on AEN.**  V2N's default carve-out is
**non-cacheable** because the M33-SM has no data cache.  AEN's M55
cores **do** have a cache, so the default flips to **cacheable** with
auto-generated cache-maintenance points in `alp_rpc_*`.  Don't write
cache ops by hand.  If you reach below the RPC surface to read shared
memory directly, you have to call the right cache ops yourself — and
the right calls differ between V2N and AEN.  Setting `cacheable: true`
in `ipc:` explicitly makes the orchestrator emit matching hooks on
both sides.

**Mailbox-channel collisions.**  The SoM preset declares which
controller channels the SDK reserves vs. leaves free for apps:

```yaml
# metadata/e1m_modules/E1M-V2N101.yaml
mailbox:
  channels:
    - { id: 0, reserved_for: alp_default_rpmsg }
    - { id: 1, reserved_for: app }
    - { id: 2, reserved_for: app }
    - { id: 3, reserved_for: power_mgmt }
```

Adding a second `rpmsg` channel with no free
`reserved_for: alp_default_rpmsg` slot fails at validate time.  Don't
override `reserved_for: power_mgmt` — that channel carries the PMIC's
runtime power-state machine.

**Forgetting `app:` is relative to the project root.**  `app: ./linux`
resolves to `<project_root>/linux/`, not to wherever
`west alp-build` is invoked from.  Always pass the workspace-relative
path of the project as the build argument:

```bash
# good
west alp-build examples/multicore/rpmsg-v2n
```

The orchestrator writes `build/` next to the project's `board.yaml`.

## 11. Next steps

- **Full design rationale:** the spec at
  `docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`
  covers the metadata layer, the loader rules, and the boot-order
  semantics in depth.
- **ADR:** [`docs/adr/0010-heterogeneous-os-orchestration.md`](adr/0010-heterogeneous-os-orchestration.md)
  records the decision and its alternatives.
- **Low-level primitives:** [`include/alp/mproc.h`](../include/alp/mproc.h)
  is what `<alp/rpc.h>` sits on (shmem + mailbox + hwsem).  Reach
  for these only if you're building a custom framing layer.
- **Advanced example:** `examples/multicore/heterogeneous-offload/` shows the
  A-cluster delegating an FFT computation to the M peer over RPMsg —
  useful when you want CPU-vs-NPU-vs-M-class compute routing in one
  project.
- **Adding a new SoM:** [`docs/porting-new-som.md`](porting-new-som.md)
  covers authoring a SoM preset's `topology:` (required),
  `mailbox:` (required), and `memory_map:` (optional — non-stock
  partitioning only) blocks for silicon the SDK hasn't met yet.
