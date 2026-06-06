# Heterogeneous OS orchestration — Zephyr + Yocto as peers, not alternatives

**Status:** Design approved 2026-05-15 · Pre-implementation
**Owner:** alpCaner
**Replaces:** `board.yaml` v1 single-OS model
**Lands in:** v0.6 (target 2026-Q3)

---

## 1. Problem statement

Today the Alp SDK presents **Zephyr** and **Yocto** as mutually-exclusive build
targets selected by a single `os:` field in `board.yaml`. That framing is
architecturally wrong for the SoMs the SDK actually ships against:

- **E1M-V2N101** carries 4× Cortex-A55 (Linux-class) **and** 1× Cortex-M33
  system-manager (RTOS-class) on the same RZ/V2N die.
- **E1M-AEN701** carries 2× Cortex-A32 **and** 2× Cortex-M55 (HP + HE) on the
  same Alif Ensemble E7 die.
- **E1M-NX9101** carries 2× Cortex-A55 **and** 1× Cortex-M33 on i.MX 93.

A real product running on one of these SoMs needs Yocto on the A-cluster *and*
Zephyr on the M-class peer, talking over IPC. The current schema forces the
customer to pick one and hand-implement the other half outside the SDK — the
existing `examples/mproc-dual-os-yocto-zephyr/` is the receipt for this:
its `board.yaml` says `os: zephyr` with a 13-line comment apologising that the
Yocto half lives in a separate bitbake recipe that does not consume the same
declarative config.

The SDK's value proposition is that the customer declares what they want and
the SDK fans out into per-vendor build systems. The current architecture
abandons that proposition the moment heterogeneous compute enters the picture.

### Audit summary (where the either/or lives)

| # | Location | Issue |
|---|---|---|
| 1 | `metadata/schemas/board-config-v1.schema.json:99-103` | `os:` is a single-string enum (`zephyr \| yocto \| baremetal`). |
| 2 | `scripts/alp_project.py` `--emit zephyr-conf \| yocto-conf \| ...` | Picks one target per invocation. |
| 3 | `examples/mproc-dual-os-yocto-zephyr/board.yaml` | Documented workaround: two halves drift independently. |
| 4 | `yocto/meta-alp/` + `meta-alp-sdk/` | Two overlapping Yocto trees. |
| 5 | `scripts/west_commands/alp.py` | West extension is Zephyr-only. |
| 6 | `docs/porting-new-som.md` §1 | Docs reinforce "pick one OS per SoM". |
| 7 | `.github/workflows/` | `pr-twister.yml` exists; no `pr-yocto-build.yml`. Yocto is unverified in PR CI. |
| 8 | `include/alp/mproc.h:50-57` | Runtime API already knows about heterogeneous cores (`ALP_CORE_M55_HP/HE/A32_0/A32_1`) — the build can't actually produce both halves. |
| 9 | `metadata/socs/.../*.json` `cores[]` | SoC metadata already lists cores per silicon. Nothing reads it for OS routing. |
| 10 | (no file) | No system-level manifest captures "which core runs which OS". That mapping lives in prose in example READMEs. |

---

## 2. Decisions

| Axis | Decision |
|---|---|
| **Scope** | All heterogeneous SoMs (V2N, AEN E5..E8, iMX93, and any future heterogeneous silicon). One generic mapping driven from `metadata/socs/.../cores[]`. |
| **IPC** | OpenAMP / RPMsg + virtio as canonical. `alp_mbox` / `alp_shmem` / `alp_hwsem` stay as the low-level primitives. `<alp/rpc.h>` (already declared as v0.3 in `mproc.h`) is the framed channel customers use. |
| **CLI** | Extend `west alp-build` to fan out across cores. New companions: `west alp-image`, `west alp-flash`, `west alp-clean`. |
| **Schema** | Rewrite `board.yaml`: per-core `cores:` block mandatory; the global `os:` enum removed.  No migration script — every in-repo `board.yaml` rewritten as part of the implementation.  (The original `schema_version: 2` marker has since been dropped — there is one live schema at `metadata/schemas/board.schema.json`.) |
| **CI** | PR-level: bitbake every A-cluster MACHINE, Zephyr-build every M-class slice, Renode dual-OS smoke test. Twister stays as the Zephyr-only fast lane. |
| **Defaults** | Heterogeneous-by-default: for every on-die programmable core, the SoM preset declares a sensible default OS + app. A bare `som: { sku: E1M-V2N101 }` produces both A55=Yocto and M33-SM=Zephyr with stock apps. Opt-out is explicit (`os: off`). |

---

## 3. Conceptual model

A project no longer targets "an OS". It targets a **system topology**: a SoM
has cores, each core runs a runtime, each runtime runs an app slice, and the
runtimes talk over IPC carve-outs.

```
metadata/socs/<vendor>/<family>/<part>.json    silicon facts (cores[], NPUs, peripherals)
        ↓
metadata/e1m_modules/<SKU>.yaml                per-SoM defaults: topology, memory_map, mailbox
        ↓
board.yaml `cores:` block                      per-project overrides + cross-core IPC carve-outs
        ↓
scripts/alp_orchestrate.py                     fans out into N sub-builds + assembles a system manifest
        ↓
build/system-manifest.yaml                     single source of truth for image + flash + OTA
```

### 3.1 Three new ideas

1. **System manifest** (`build/system-manifest.yaml`) — generated artefact
   capturing every core's binary, every IPC carve-out, the boot order, and
   pointers to helper-MCU firmware (GD32, CC3501E). Consumed by `alp-image`,
   `alp-flash`, the OTA bundler, and (eventually) alp-studio.
2. **Per-core runtime tokens** — `yocto`, `zephyr`, `baremetal`, `off`.
   `off` is a first-class state: if your firmware genuinely doesn't need the
   M33, you say so and the build skips it. No implicit "did we forget a core?"
   failure mode.
3. **Per-SoM topology preset** — `metadata/e1m_modules/<SKU>.yaml` gains a
   `topology:` block declaring the default OS + app per on-die core.
   Customers inherit + override; they don't have to know "the V2N has 4 A55s
   and 1 M33" by heart.

### 3.2 Default policy

For every on-die programmable core enumerated in
`metadata/socs/.../cores[]`:

| Core class | Default runtime | Default app if customer doesn't supply one |
|---|---|---|
| Cortex-A (A55, A32, A53) | **Yocto Linux** | `alp-image-edge` reference rootfs |
| Cortex-M (M33, M55, M7) | **Zephyr** | `alp-stock-shim` firmware (boots, opens RPMsg, idles) |
| Ethos-U / DRP-AI / DEEPX | n/a — driven by the A-class core | (built into `<alp/inference.h>`) |

A bare `som: { sku: E1M-V2N101 }` produces both A55=Yocto and M33-SM=Zephyr.
Opt-out is explicit (`cores: { m33_sm: { os: off } }`).

### 3.3 Core ID nomenclature

| SoM | `cores:` keys |
|---|---|
| V2N (RZ/V2N) | `a55_cluster`, `m33_sm` |
| AEN E5/E6/E7/E8 | `a32_cluster`, `m55_hp`, `m55_he` |
| AEN E3/E4 (no A-class) | `m55_hp`, `m55_he` |
| iMX93 | `a55_cluster`, `m33` |

Cluster cores collapse into one key (`a55_cluster`) since they run a single
Linux instance over a shared MMU. M-class cores stay separate because each is
independently programmable.

### 3.4 On-die cores vs on-module helper MCUs

The `cores:` block addresses **on-die programmable cores only**. On-module
helper MCUs (GD32 supervisor, CC3501E Wi-Fi coprocessor) are a separate
concern: their firmware builds via existing dedicated pipelines and their
outputs are *registered* into the system manifest so the customer's flash
flow covers them, but they are not heterogeneous-compute peers.

| Concept | Surface | `cores:` |
|---|---|---|
| On-die cores | `metadata/socs/.../cores[]` → `board.yaml cores:` | **Yes** |
| Helper MCUs | `metadata/e1m_modules/<SKU>.yaml on_module.supervisor_mcu` etc. | **No** |

---

## 4. Metadata layer

Three layers, each with a clear concern.

### 4.1 SoC silicon facts (`metadata/socs/<vendor>/<family>/<part>.json`)

Add a normalized `id` to each existing `cores[]` entry. No OS opinions —
the same die ships in different SoMs that may pick different defaults.

```json
"cores": [
  { "id": "a55_cluster", "type": "cortex-a55", "count": 4, "freq_mhz": 1800, ... },
  { "id": "m33_sm",      "type": "cortex-m33", "subtype": "system-manager",
                         "count": 1, "freq_mhz": 200, ... }
]
```

**Schema change:** `metadata/schemas/soc-spec-v1.schema.json` adds
`required: ["id"]` to the `core` `$def` and a regex
`pattern: "^[a-z][a-z0-9_]+$"` on `id`. The `id` must be unique within a SoC.

### 4.2 Per-SoM defaults (`metadata/e1m_modules/<SKU>.yaml`)

Three new blocks: `topology:`, `memory_map:`, `mailbox:`.

```yaml
# metadata/e1m_modules/E1M-V2N101.yaml

topology:
  a55_cluster:
    os: yocto
    app: alp-image-edge
    machine: e1m-v2n101-a55           # Yocto MACHINE
    toolchain: poky-glibc
  m33_sm:
    os: zephyr
    app: alp-stock-shim
    board: alp_e1m_v2n101_m33_sm      # Zephyr board target
    toolchain: arm-zephyr-eabi

memory_map:
  - { name: ddr_main,  base: 0x48000000, size_mib: 4096, accessible_from: [a55_cluster, m33_sm], cacheable: true  }
  - { name: ocram_low, base: 0x00010000, size_kib:  512, accessible_from: [a55_cluster, m33_sm], cacheable: false }
  - { name: m33_tcm,   base: 0x80000000, size_kib:  128, accessible_from: [m33_sm],               cacheable: false }

mailbox:
  controller: renesas_mhu
  channels:
    - { id: 0, reserved_for: alp_default_rpmsg }
    - { id: 1, reserved_for: app }
    - { id: 2, reserved_for: app }
    - { id: 3, reserved_for: power_mgmt }
```

`topology:` carries one entry per `cores[].id` in the resolved SoC spec.
**Validation invariant:** every key in `topology:` MUST map to a `cores[].id`
in the SoC spec; missing keys produce a loader error at validate time.

### 4.3 Per-project mapping (`board.yaml` v2)

```yaml
som:
  sku: E1M-V2N101
  hw_rev: r1

preset: e1m-x-evk
cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb, emmc]
    libraries:   [mbedtls, nlohmann_json]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    app: ./m33                                # os: omitted (topology default)
    peripherals: [adc, pwm, i2c, gpio]
    libraries:   [cmsis_dsp]
    inference:   { default_arena_kib: 64 }    # arena tuning only

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics: { log_level: info }
```

### 4.4 Schema changes vs v1

| v1 | v2 |
|---|---|
| top-level `os:` (enum) | **removed** |
| top-level `peripherals: / libraries: / inference: / iot:` | moved **per-core** under `cores.<id>` |
| `board.populated:` + `chips:` | **unchanged** — describe physical assembly, shared across cores |
| `diagnostics:` | **unchanged** — applies project-wide |
| (none) | new top-level `cores:` block (required) |
| (none) | new top-level `ipc:` block (optional) |

### 4.5 Loader rules

In `scripts/alp_project.py`:

1. For each `id` in `metadata/socs/.../cores[]` of the resolved SKU:
   - If `cores.<id>` is set in the project → use that.
   - Else fall back to `topology.<id>` from the SoM preset.
   - If neither exists → loader error: `core '<id>' has no runtime assigned`.
2. `os: off` skips the slice entirely (no `app:` required).
3. `os: zephyr` requires `app:` pointing at a directory containing `prj.conf`
   (or `CMakeLists.txt` for OS-less / app-as-CMake-project).
4. `os: yocto` requires either `app:` (custom CMake project recipe) or
   `image:` (override the stock recipe).
5. `os: baremetal` requires `app:` pointing at a CMake project.
6. Every `ipc[]` entry's `endpoints[]` must all resolve to cores with
   `os: != off`.
7. `cores:` keys are validated against the SoM preset's `topology:` keys —
   `cores: { m99: ... }` on a V2N fails at validate time.

### 4.6 New `alp_project.py` emit modes

| Mode | Output | Consumer |
|---|---|---|
| `zephyr-conf --core <id>` | per-core Kconfig fragment | Zephyr build (replaces `--emit zephyr-conf`) |
| `yocto-conf --core <id>` | per-core `local.conf` snippet | bitbake (replaces `--emit yocto-conf`) |
| `system-manifest` | `system-manifest.yaml` | `alp-image`, `alp-flash`, OTA |
| `dts-reservations` | `dts-reservations.dtsi` | included by Linux kernel DT + Zephyr DT |
| `ipc-contract-h` | `alp/system_ipc.h` | `#include <alp/system_ipc.h>` from every slice's C code |

The existing `cmake-args`, `dts-overlay`, `hw-info-h`, `west-libraries`
emit modes stay; `zephyr-conf` / `yocto-conf` gain the `--core <id>` arg.

---

## 5. Build orchestrator

One Python core (`scripts/alp_orchestrate.py`), multiple west wrappers
(`scripts/west_commands/alp_*.py`).

### 5.1 West commands

| Command | Purpose |
|---|---|
| `west alp-build` | Read `board.yaml`, fan out per-core slices, emit `system-manifest.yaml`. |
| `west alp-image` | Consume the manifest → assemble a single flashable bundle (`build/image-bundle/`). |
| `west alp-flash` | Walk the manifest, program each slice via the right backend tool. |
| `west alp-clean` | Tear down all slice build dirs + orchestrator state. |
| `west alp-renode` | Boot the system manifest in Renode (heterogeneous smoke test). |

### 5.2 Fan-out algorithm

```
1. Load + validate board.yaml against v2 schema.
2. Resolve SoM preset → topology defaults → effective per-core mapping.
3. For each core C with os != off:
     - Materialize per-core config:
         • Zephyr   → build/<C>-zephyr/alp.conf
         • Yocto    → build/<C>-yocto/local.conf
         • Baremetal→ build/<C>-baremetal/cmake-args.txt
     - Resolve toolchain + board target from topology.<C>:
         • Zephyr board names: alp_<sku>_<core_id>
         • Yocto MACHINE:      e1m-<sku>-<core_id_class>
4. Emit shared generated artefacts:
     - build/generated/alp/system_ipc.h       (matches `#include <alp/system_ipc.h>`)
     - build/generated/dts-reservations.dtsi  (carve-out reservations)
     - build/generated/alp_hw_info_build.h    (existing — unchanged)
5. Register helper-MCU artefacts (GD32, CC3501E) by invoking existing builds.
6. Dispatch slice builds in parallel (concurrent.futures.ProcessPoolExecutor);
   each slice gets its own subprocess + scoped PATH/env.
7. Join + write build/system-manifest.yaml:
     - per-slice: { core_id, os, app, output_artefact, status, log_path }
     - ipc[]:     { kind, endpoints, carve_out_addr, carve_out_size, rpmsg_endpoint_ids }
     - helper_mcus[]: { name, firmware_path, flash_method }
     - boot_order: [ ... ] (from SoM preset)
     - hw_info:   { sku, hw_rev, board, ... }
```

### 5.3 Output directory layout

```
build/
├── a55_cluster-yocto/
│   ├── conf/local.conf
│   └── tmp/deploy/images/<MACHINE>/{rootfs.wic.gz, Image, *.dtb}
├── m33_sm-zephyr/
│   └── zephyr/zephyr.elf
├── helper-gd32/
│   └── gd32_bridge.bin
├── helper-cc3501e/
│   └── cc3501e_otp.blob
├── generated/
│   ├── alp/system_ipc.h           (matches `#include <alp/system_ipc.h>`)
│   ├── dts-reservations.dtsi
│   └── alp_hw_info_build.h
└── system-manifest.yaml
```

### 5.4 UX examples

```bash
# Default — build everything board.yaml declares:
west alp-build examples/multicore/rpmsg-v2n

# Iterate on one slice only (skips Yocto's hour-long rebuild):
west alp-build examples/multicore/rpmsg-v2n --core m33_sm

# Bundle for flashing or OTA:
west alp-image     # → build/image-bundle/alp-system.zip + .swu (Mender)

# Program attached hardware:
west alp-flash     # respects boot_order: from the manifest

# Heterogeneous smoke test in Renode (CI uses the same command):
west alp-renode
```

### 5.5 Parallelism + caching

- Slice builds run in parallel — they share no build-time dependencies because
  the IPC contract is materialised as generated headers consumed at compile
  time. The orchestrator dispatches via `ProcessPoolExecutor` and joins at the
  end.
- Each slice has scoped PATH + env so toolchains don't collide.
- Per-slice `ccache` (Zephyr/baremetal) + per-slice `sstate-cache` (Yocto).
- Orchestrator persists `build/.alp-build-state.json` so re-runs only rebuild
  slices whose inputs changed.
- Slice failures don't cascade. `system-manifest.yaml` carries per-slice
  `status: ok | failed`. Exit code is non-zero if any slice failed, but
  successful slices stay built so iteration is cheap.

### 5.6 Single-OS projects keep working

A `board.yaml` that declares only `cores: { m55_hp: { os: zephyr, app: ./src } }`
on AEN E3 (no A-class) produces a single-slice fan-out — same behaviour as
today's `os: zephyr` path, just routed through the orchestrator.
Zephyr-only customers see no change in their build experience.

---

## 6. IPC contract

**Single principle:** every cross-core touchpoint is generated from the
`ipc[]` block + the SoM preset's memory map. Both kernels' DTs and both
apps' headers are emitted from the same source. Manual sync ends.

### 6.1 Carve-out address resolution

For each `ipc[]` entry in the project's `board.yaml`, the resolver
(`alp_orchestrate.py:resolve_carve_outs`):

1. Filters `memory_map[]` regions whose `accessible_from:` covers every
   endpoint listed.
2. Prefers non-cacheable regions (ocram on V2N) unless the entry sets
   `cacheable: true`.
3. Allocates from the top of the region down, page-aligned (4 KiB Linux side;
   MPU-aligned M-class side).
4. Records the resolved `base` + `size` into `system-manifest.yaml` and into
   every emitted artefact.

**Blocked carve-outs.** When the SoM preset still carries TBD
`mailbox.controller` or TBD `memory_map.base` / `size` (the common
case while a SoM is being HW-mapped), the resolver emits the carve-out
as `status: blocked` + `reason: ...` instead of raising. The manifest
remains emit-able, which keeps the CI manifest-shape gate green while
preset metadata is in flight. The generated `<alp/system_ipc.h>`
carries an `#error` directive for any blocked channel, so the actual
slice-build step is what trips — never the manifest emission.

**Determinism.** Sort `ipc[]` entries alphabetically by `name:` before
allocating. Re-running the build produces byte-identical layouts; CI
diffs the manifest. Wall-clock / PID-style data (per-slice
`duration_s`, etc.) lives on the runtime Slice dataclass but never
lands in the manifest — the orchestrator strips it from
`to_manifest_entry()` so the output stays content-addressable across
rebuilds.

### 6.2 DT reservations (`dts-reservations.dtsi`)

```dts
/ {
  reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;

    alp_default_rpmsg: alp_default_rpmsg@10078000 {
      compatible = "shared-dma-pool";
      reg = <0x0 0x10078000 0x0 0x80000>;
      no-map;
      label = "alp_default_rpmsg";
    };
  };
};
```

- Linux side: a new bitbake recipe `alp-dts-reservations` includes this
  `.dtsi` into the kernel DT under `IMAGE_BOOT_FILES` for every `e1m-<sku>-a*`
  MACHINE.
- Zephyr side: the alp-sdk Zephyr module's
  `boards/<sku>_<core_id>.overlay` `#include`s the same file.

### 6.3 RPMsg endpoint allocation (`alp/system_ipc.h`)

```c
/* Auto-generated by alp_orchestrate.py.  Do not edit. */
#define ALP_IPC_ALP_DEFAULT_RPMSG_NAME       "alp_default_rpmsg"
#define ALP_IPC_ALP_DEFAULT_RPMSG_ADDR       0x00010000u
#define ALP_IPC_ALP_DEFAULT_RPMSG_SIZE       0x00080000u
#define ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT    0x000004e6u
#define ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT    0x000004e7u
#define ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH    0u
```

- The macro stem is `ALP_IPC_` + the channel `name:` upper-cased
  (so `alp_default_rpmsg` → `ALP_IPC_ALP_DEFAULT_RPMSG_*`).
- Endpoint IDs: `0x400 | (FNV-1a(ipc.name) & 0x0FF)` for src, `+1` for dst.
- Stable across rebuilds, collision-checked at emit time (loader error if two
  `ipc[]` entries hash to the same low byte).
- The header is `#include`d by **both** the Zephyr/M-side app and the
  Linux/A-side app — no possibility of mismatched endpoint IDs.
- When the carve-out is blocked (§6.1), the entry's `#define ..._NAME`
  is still emitted but accompanied by an `#error` directive, so any
  consumer that `#include`s the header trips at compile time until
  the preset is unblocked.

### 6.4 Mailbox channel reservation

Each SoM preset's `mailbox.channels[]` declares which controller channels are
reserved by the SDK vs free for app use. If an `ipc[]` entry asks for
`kind: rpmsg` and no channel is `reserved_for: alp_default_rpmsg`, the
resolver emits the entry as `status: blocked` so reviewers see the gap
in the manifest; the slice build is what fails (via the
`<alp/system_ipc.h>` `#error`).

### 6.5 remoteproc firmware contract

The orchestrator emits a `meta-alp-sdk` bbappend that installs
`build/m33_sm-zephyr/zephyr/zephyr.elf` to
`/lib/firmware/alp/<sku>/m33_sm.elf` in the rootfs. The kernel DT carries:

```dts
&m33_remoteproc {
  status = "okay";
  firmware-name = "alp/E1M-V2N101/m33_sm.elf";
  mboxes = <&mhu 0>, <&mhu 1>;
  memory-region = <&alp_default_rpmsg>;
};
```

`meta-alp-sdk/recipes-core/alp-system/alp-remoteproc.service` (new systemd
unit) runs `echo start > /sys/class/remoteproc/remoteproc0/state` at boot.
The M-side firmware enters `main()` with RPMsg endpoints already
discoverable via OpenAMP's resource table.

### 6.6 Runtime API surface

App code stays at the existing `<alp/...>` level:

```c
#include <alp/system_ipc.h>      /* generated */
#include <alp/rpc.h>             /* OpenAMP-backed framed RPC */

static void on_temperature(float c, void *u) { /* ... */ }

int main(void) {
    alp_rpc_channel_t *ch = alp_rpc_open(&(alp_rpc_config_t){
        .name      = ALP_IPC_ALP_DEFAULT_RPMSG_NAME,
        .src_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_SRC_EPT,
        .dst_ept   = ALP_IPC_ALP_DEFAULT_RPMSG_DST_EPT,
        .mbox_ch   = ALP_IPC_ALP_DEFAULT_RPMSG_MBOX_CH,
    });
    alp_rpc_subscribe(ch, "temperature", on_temperature, NULL);
}
```

Customer never types an address, endpoint ID, or mailbox channel by hand.
`<alp/rpc.h>` sits on OpenAMP: Zephyr's `subsys/ipc/rpmsg_service` on the
M-side, `librpmsg-lite` or libmetal on the Linux side.

### 6.7 Boot order

Recorded in the SoM preset, copied verbatim into `system-manifest.yaml`:

```yaml
boot_order:
  - { stage: 1, core: a55_cluster, action: u_boot_to_kernel_via_xspi }
  - { stage: 2, core: a55_cluster, action: systemd_target_basic }
  - { stage: 3, core: m33_sm,      action: remoteproc_start, by: a55_cluster }
  - { stage: 4, ipc:  alp_default_rpmsg, action: rpmsg_handshake }
```

AEN inverts (M55-HP boots first from MRAM, then optionally brings up A32 via
custom bootloader). The orchestrator reads, never assumes.

### 6.8 Cache coherency

Default carve-out is **non-cacheable** because V2N's M33-SM has no cache and
the A55 coherency unit is configurable at SoC boot. Customers who set
`cacheable: true` get cacheable regions + the resolver verifies the A55 +
M-class peer both have the required MMU/MPU attributes. AEN's M55 cores have
cache, so on AEN the default flips to `cacheable: true` with explicit
cache-maintenance points generated into `alp_rpc_*` (the SDK calls
`clean+invalidate` at the right edges; customers don't write cache ops by
hand).

---

## 7. Implementation phases

Five phases. Each ships as a separate PR against `main`, fully green in CI
before the next opens.

### Phase 1 — Metadata + schema

- Add `id` to every entry in `metadata/socs/<vendor>/<family>/<part>.json`
  `cores[]`. Update `metadata/schemas/soc-spec-v1.schema.json` to require it.
- Add `topology:` + `memory_map:` + `mailbox:` blocks to every SoM preset in
  `metadata/e1m_modules/<SKU>.yaml`.
- Author `metadata/schemas/board.schema.json` with the per-core
  `cores:` block + `ipc:` block. **Delete** `board-config-v1.schema.json`.
- Update `metadata/schemas/som-preset-v1.schema.json` to validate the new
  topology/memory_map/mailbox blocks.

### Phase 2 — Orchestrator core

- New `scripts/alp_orchestrate.py` (fan-out + manifest emission).
- Extend `scripts/alp_project.py` with new emit modes
  (`system-manifest`, `dts-reservations`, `ipc-contract-h`) and `--core <id>`.
- New west wrappers: `scripts/west_commands/alp_build.py`,
  `alp_image.py`, `alp_flash.py`, `alp_clean.py`, `alp_renode.py`.
  Update `scripts/west-commands.yml` + `zephyr/module.yml`.
- Update `scripts/validate_board_yaml.py` to load v2 schema.

### Phase 3 — IPC contract

- Generators in `alp_orchestrate.py`:
  `alp/system_ipc.h`, `dts-reservations.dtsi`, remoteproc bbappend.
- Zephyr OpenAMP/RPMsg backend: `src/zephyr/rpc_zephyr.c` implementing
  `<alp/rpc.h>` on `subsys/ipc/rpmsg_service`.
- Linux OpenAMP backend: `src/yocto/rpc_yocto.c` on `librpmsg` or libmetal.
- `meta-alp-sdk/recipes-core/alp-system/alp-remoteproc.service` systemd unit.
- Per-SoC `boards/<sku>_<core_id>.overlay` files under `zephyr/boards/`.

### Phase 4 — Rewrite the world

- Convert every `board.yaml` in the repo (~32 files) to v2.
- Rename `examples/mproc-dual-os-yocto-zephyr/` → `examples/multicore/rpmsg-v2n/`,
  delete the 13-line workaround comment, restructure into `linux/` + `m33_sm/`
  sub-directories.
- Add `examples/multicore/rpmsg-aen/` (A32 + M55-HP heterogeneous), `examples/multicore/rpmsg-imx93/`,
  `examples/multicore/heterogeneous-offload/` (A-cluster delegates FFT to M peer).
- **Delete** `yocto/meta-alp/`. Move any unique content into `meta-alp-sdk/`
  before deletion (verify nothing else is lost).
- Update `src/yocto/` references in the codebase if any point at the old layer.

### Phase 5 — CI + docs

- New workflows:
  - `pr-alp-build.yml` (3 SoMs × 3 scenarios; verifies `system-manifest`
    determinism).
  - `pr-bitbake.yml` (self-hosted runner, sstate-cache-backed).
  - `pr-renode-dual-os.yml` (V2N A55 + M33-SM heterogeneous boot + RPMsg
    handshake test).
- Extend `pr-twister.yml` to invoke the orchestrator's `--core <id>` mode for
  Zephyr-only slices.
- Extend `pr-metadata-validate.yml` for v2 schema + cross-checks (topology vs
  SoC `cores[].id`).
- New doc: `docs/adr/0010-heterogeneous-os-orchestration.md`.
- New doc: `docs/heterogeneous-builds.md` (app-developer how-to).
- Restructure `docs/os-support-matrix.md` columns from "SoM / OS" to
  "SoM core × OS".
- Update `README.md`: kill "Selected by `os:` in `board.yaml`"; rewrite the
  30-second quick start with a per-core example; add "What runs where" table.
- Update `docs/porting-new-som.md` §1: "Decide your OS targets" → "Confirm
  core topology + default OS mapping".
- Update `docs/glossary.md`: add *core id*, *topology block*, *system manifest*,
  *carve-out*, *slice*.
- Update `docs/zephyr-version-policy.md`: clarify Zephyr pin applies
  per-Zephyr-slice; Linux kernel version set by Yocto BSP, not coupled.

---

## 8. Examples convention

Heterogeneous examples gain per-core sub-directories named after the
`cores:` keys.

```
examples/multicore/rpmsg-v2n/
├── board.yaml                       (v2; declares a55_cluster + m33_sm)
├── README.md
├── linux/                           (a55_cluster's app)
│   ├── CMakeLists.txt
│   └── src/main.c                   (consumer using <alp/rpc.h>)
└── m33_sm/                          (m33_sm's app)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c                   (producer using <alp/rpc.h>)
```

Single-OS examples are **unchanged** — they just gain a per-core block in
their `board.yaml`. The dual-OS framing is opt-in per project.

| Path | Topology |
|---|---|
| `examples/multicore/rpmsg-v2n/` | V2N — A55 Yocto consumer + M33-SM Zephyr producer |
| `examples/multicore/rpmsg-aen/` (new) | AEN E7 — A32 Yocto + M55-HP Zephyr |
| `examples/multicore/rpmsg-imx93/` (new) | iMX93 — A55 Yocto + M33 Zephyr |
| `examples/multicore/heterogeneous-offload/` (new) | A-cluster delegates FFT to M peer over RPMsg |

---

## 9. Cleanup

| Action | Reason |
|---|---|
| Delete `yocto/meta-alp/` | Overlapping older Yocto layer; `meta-alp-sdk/` is the single source. |
| Delete `metadata/schemas/board-config-v1.schema.json` | v2 fully replaces it; no migration needed. |
| Remove `os:` enum from all schemas | Per-core mapping replaces the global field. |
| Remove the 13-line workaround comment from `examples/mproc-dual-os-yocto-zephyr/board.yaml` | The example now reflects reality after rename. |
| Retire "Zephyr OR Yocto" framing in all customer-facing docs | Misleading; replace with per-core narrative. |

---

## 10. Risks + open questions

| Risk | Mitigation |
|---|---|
| Bitbake build time blows up PR CI | Self-hosted runner with persistent sstate-cache. Cold build ~hours; warm ~5 min steady-state. Spin-up cost is a one-time investment. |
| Renode dual-OS smoke test is fragile | Start with the simplest possible handshake (RPMsg name service ping/pong). Expand only after the baseline is reliable. |
| Per-SoM memory maps may diverge from vendor reference | Document `memory_map:` source-of-truth in each preset's comments; cross-check against vendor reference manuals in the metadata PR. |
| AEN E3/E4 (no A-class) regresses behind the heterogeneous path | Single-slice fan-out handles this — verify in CI matrix that E3 zephyr-only builds stay byte-stable vs the pre-orchestrator artefact. |
| Boot-order semantics drift between SoMs | Each preset's `boot_order:` is the contract. Encode validation: every IPC handshake stage must reference a `core:` that's already at `remoteproc_start` or later in the order. |

**Open questions** (deferred to implementation):

- **`<alp/rpc.h>` framing format.** Use OpenAMP's name-service + arbitrary
  payload, or layer a fixed framing on top (length-prefixed binary, or
  protobuf-style via nanopb)? Lean: name-service + opaque payload for v0.6,
  protobuf framing as an opt-in upgrade in v0.7.
- **Helper-MCU flashing tool selection.** The `alp-flash` command needs a
  per-helper-MCU backend table (openocd for GD32 via SWD, USB-CDC bootloader
  for CC3501E). Lean: define the table in the SoM preset's `topology:` next
  to each helper MCU entry.
- **`alp-image` bundle format.** Plain tar + manifest, or a versioned binary
  format that includes signatures? Lean: tar + manifest for v0.6 (the OTA
  bundler already does signing for Mender's `.swu` format).

---

## 11. Acceptance criteria

This design is delivered when:

- [ ] Every `board.yaml` in the repo is v2 and validates against the new schema.
- [ ] `west alp-build examples/multicore/rpmsg-v2n` produces a non-trivial
      `system-manifest.yaml` with both `a55_cluster-yocto` and
      `m33_sm-zephyr` slices present and `status: ok`.
- [ ] `west alp-renode` boots the V2N system manifest, completes the RPMsg
      handshake, and exits 0 in CI.
- [ ] `pr-alp-build.yml` + `pr-bitbake.yml` + `pr-renode-dual-os.yml` are all
      green on `main`.
- [ ] No `board.yaml` in the repo carries the v1 `os:` field.
- [ ] `yocto/meta-alp/` directory no longer exists.
- [ ] `docs/os-support-matrix.md` has been restructured to per-core columns.
- [ ] ADR 0010 lands.
- [ ] `docs/heterogeneous-builds.md` walks an app developer through writing a
      dual-app project end-to-end.

---

## 12. References

- `include/alp/mproc.h` — existing low-level IPC primitives (kept as-is).
- `metadata/socs/renesas/rzv2n/n44.json` — example of SoC `cores[]` shape
  this design extends.
- `metadata/e1m_modules/E1M-V2N101.yaml` — example of SoM preset this design
  extends.
- `scripts/alp_project.py` — existing loader, extended in Phase 2.
- `scripts/west_commands/alp.py` — existing `west alp-build`, restructured
  in Phase 2 into multiple commands.
- `examples/mproc-dual-os-yocto-zephyr/` — current dual-OS workaround, the
  reference for what Phase 4 fixes.
- OpenAMP project: <https://www.openampproject.org/>.
- Zephyr `subsys/ipc/rpmsg_service` documentation.
- Linux remoteproc + RPMsg subsystem documentation.
