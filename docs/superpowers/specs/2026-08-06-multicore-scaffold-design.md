# Multi-core scaffolding for dual-Zephyr-core SoMs — design

Date: 2026-08-06
Issue: [#1275](https://github.com/alplabai/alp-sdk/issues/1275) item 1
Status: design approved; implementation not started

## Problem

E1M-AEN801 carries two Zephyr-capable Cortex-M55 cores (`m55_hp`, `m55_he`) —
both declare a `board:` in the SoM preset's `topology:`. Scaffolding a stock
template onto that SKU produces **one** source tree bound to one core. The
second M55 falls back to `firmware/alp-stock-shim`, the SDK-owned idle image.

Measured before this design: **zero of 99 example `board.yaml` declared two
explicit `os: zephyr` cores.** The dual-Zephyr-core path the SoM actually ships
was unexercised across the whole corpus.

As a SoM vendor shipping a dual-M55 part, a working dual-core story is not
optional. This design makes scaffolding produce a real two-core project.

## Ground truth this design is built on

Three sources, in decreasing authority. Nothing here is inferred.

### Alif DFP — `alifsemi/alif_ensemble-cmsis-dfp` (public)

`AlifSemiconductor.Ensemble.pdsc`, device **AE822FA0E5597LS0**:

| region | start | size |
|---|---|---|
| MRAM | `0x80000000` | `0x00580000` (5632 KiB) |
| SRAM0 | `0x02000000` | `0x00400000` (4 MiB) |
| SRAM2 — M55-HP ITCM | `0x50000000` | `0x00040000` (**256 KiB**) |
| SRAM3 — M55-HP DTCM | `0x50800000` | `0x00100000` (1024 KiB) |
| SRAM4 — M55-HE ITCM | `0x58000000` | `0x00040000` (**256 KiB**) |
| SRAM5 — M55-HE DTCM | `0x58800000` | `0x00040000` (256 KiB) |

`Boards/DevKit-e8/Examples/DualCore/.alif/M55_HP_HE_mram_cfg.json`:

| entry | `cpu_id` | address | flags |
|---|---|---|---|
| `HE_APP` | `M55_HE` | `mramAddress 0x80000000` | `["boot"]` |
| `HP_APP` | `M55_HP` | `mramAddress 0x80200000` | `["boot"]` |
| `A32_STUB` | `A32_0` | `loadAddress 0x02000000` | `["load","boot"]` |

Two facts follow:

1. **HE is ordered low.** This confirms the rationale already recorded in
   `metadata/e1m_modules/E1M-AEN801.yaml:108-112`.
2. **Alif does not use `deferred`.** Both its entries are plain `["boot"]`. The
   `deferred` + Secure-Enclave un-defer mechanism is Alp Lab's own, invented
   because resetting M55-HP invalidates its TCM. **We own its correctness.**

The DFP itself is under the Alif Software License Agreement and is not
vendorable into this repo; only values and offsets are transcribed here.
`hal_alif` (Apache-2.0) remains the consumed substitute.

### `alplabai/e1m-aen-dualcore-demo` — the reference implementation

Apache-2.0, own-authored, no vendor source vendored. Bench-verified
**2026-08-04 on E1M-AEN801 (`AE822FA0E5597LS0`): 177 consecutive PING/PONG
exchanges with no gaps, surviving a cold power cycle with no debugger.**

- Apps named **by role** — `apps/dualcore_host` / `apps/dualcore_remote` —
  "because which physical cluster runs the host is a per-silicon fact".
- HOST on RTSS-HE, REMOTE on RTSS-HP, in the proven configuration.
- RPMsg over `ipc_service` (`zephyr,ipc-openamp-static-vrings`) + OpenAMP,
  doorbell = out-of-tree **MHUv2** mbox driver (`modules/alif-mhuv2`).
- MHU TX `0x400B0000`, RX `0x400A0000`, GIC IRQ **43**, priority 3.
- `sram_ipc0` at `0x02010000`, 64 KB, tagged `ATTR_MPU_RAM_NOCACHE` — the DTS
  comment calls that attribute load-bearing: without it the two cores' private
  D-cache copies of the vrings diverge and IPC hangs intermittently rather than
  failing loudly.
- Peer released at runtime by the host via Secure Enclave
  `SERVICES_boot_process_toc_entry` (service 500) against a `deferred` entry.
- **Two `west build` invocations, no sysbuild.**
- Peer is **ITCM-linked**; the ATOC peer entry is a load entry at
  `loadAddress 0x50000000`.

### alp-sdk today

- Five `examples/aen/aen-dualcore-*` apps: one tree, role chosen by
  `#if defined(CONFIG_BOARD_ALP_E1M_AEN801_M55_HP)`, **no `board.yaml`** — they
  sit entirely outside the orchestrator path. All `build_only: true` in CI,
  bench-verified separately.
- `examples/multicore/mproc-mailbox`: `src/` (HP) + `peer/` (HE), `board.yaml`
  driven, `raw_shmem` + hardware mailbox, image-wide `CONFIG_DCACHE=n`.
- `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_HP` already exists
  (set by `aen-dualcore-he-master`) — the SE deferred-peer path is partly built.
- **`alplabai/alp-zephyr-modules` is an empty single-commit scaffold** pinned to
  Zephyr v3.7.0. The "v0.4 dual-image build flow" that `mproc-mailbox`'s README
  cites as living there **does not exist**. That doc claim must be corrected.

## Decisions

**D1 — Absorb the mechanism into alp-sdk.** The two modules, the ATOC/deferred
boot semantics, the ITCM-linked peer slice, and the combined-ATOC flash all
become SDK-owned. Rationale: only this makes scaffold → build → flash → runs
true end to end, which is what a SoM vendor owes a customer on a dual-core part.

**D2 — Land the canonical example first, scaffold second.** The template system's
own invariant is "the template does NOT copy/fork this example — it IS this
example". Scaffold can only project an example that exists.

**D3 — Role-named trees, not core-named.** `app_host` / `app_remote`, per the
reference implementation's stated reasoning. This **rejects #1275 item 1's
proposed `src_m55_he/` naming**, and the rejection is deliberate: core-named
directories bake a per-silicon fact into a customer's directory layout.

**D4 — Role is a `board.yaml` choice.** Both directions are supported; the
customer declares which core hosts. Both are already exercised in-tree
(`aen-dualcore-master` = HP host, `aen-dualcore-he-master` = HE host).

**D5 — No sysbuild.** Two `west build` invocations from one project, through the
orchestrator's existing multi-slice path. Matches the reference implementation
and all five `aen-dualcore-*` apps.

**D6 — Targeted cache attribute, not image-wide.** Generate the `sram_ipc0`
`ATTR_MPU_RAM_NOCACHE` tag rather than `mproc-mailbox`'s `CONFIG_DCACHE=n`.
Same correctness, no whole-image cache penalty.

## Ownership split

**Silicon facts → `metadata/e1m_modules/E1M-AEN801.yaml` (vendor-owned):** MHU
TX/RX bases and IRQ, `sram_ipc0` base/size/attribute, ITCM and DTCM global
aliases per core, the SE release service, MRAM slot layout, and which cluster
the resident ATOC boots.

**Customer facts → the project's `board.yaml`:**

```yaml
som: {sku: E1M-AEN801}
preset: e1m-evk
cores:
  m55_he: {app: ./app_host}
  m55_hp: {app: ./app_remote}
ipc:
  kind: rpmsg
  host: m55_he
  endpoints: [m55_he, m55_hp]
```

That is the whole customer surface. Roles, deferred-boot flags, ITCM linkage,
MHU wiring and cache attributes all derive — consistent with the existing rule
that silicon-determined fields are not customer-facing.

`ipc.kind: rpmsg` between two M-cores is new; today `rpmsg` covers the
Linux↔M55 case and `mproc-mailbox` uses `raw_shmem`.

## What the emitter generates

**Both cores:** the `ipc0` node (`zephyr,ipc-openamp-static-vrings`,
`mboxes = <&mhu_tx 0>, <&mhu_rx 0>`, `memory-region = <&sram_ipc0>`), the
`sram_ipc0` carve-out with `ATTR_MPU_RAM_NOCACHE`, and `CONFIG_MBOX=y` /
`CONFIG_MBOX_ALIF_MHUV2=y` / `CONFIG_IPC_SERVICE=y` /
`CONFIG_IPC_SERVICE_BACKEND_RPMSG=y` / `CONFIG_OPENAMP=y`.

**Host core:** `role = "host"`, `CONFIG_ALIF_SE_BOOT=y`, MRAM-XIP linked at its
own slot0, ATOC entry `["boot"]`.

**Peer core:** `role = "remote"`, ITCM-linked at that core's global alias
(`m55_hp` → `0x50000000`, `m55_he` → `0x58000000`), ATOC entry carrying the
deferred flags, and the generalised
`CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC_PEER_IS_<CORE>`.

**Asymmetry to encode, not assume:** the two directions do not use the same ATOC
flags in our own tree. The reference implementation (HE host) gives the peer
`["load","boot","deferred"]`. `aen-dualcore-master` (HP host) gives its HE peer
`["load"]` — no `boot`, no `deferred`. These are different release mechanisms.
Each direction's pattern must be bench-confirmed before it becomes a default.

## Build and flash

**Build:** two `west build` invocations from one project. The peer slice is a new
slice kind — ITCM-linked, adding the `itcm.overlay` equivalent to
`DTC_OVERLAY_FILE`.

**Flash:** one combined ATOC. `tan flash` stages both images plus a generated
ATOC JSON, runs `app-gen-toc`, writes with `app-write-mram`. SETOOLS stays
licence-gated and customer-supplied (#353).

**Selective flash.** `tan flash` must support both cores or a chosen subset:

| invocation | behaviour |
|---|---|
| `tan flash` | both cores — full ATOC, both binaries staged, one write |
| `tan flash --core <id>` | **full ATOC with both entries**, only that core's binary re-staged |
| either, peer artifact missing | **refuse**, naming which core's image is absent |

> **Hazard.** The ATOC is a whole-device table. Regenerating it from only the
> selected core would drop the peer's entry and leave that core unbootable —
> the same class as the #1069 overwrite, from the other direction. Selective
> flash means "rebuild the full table, re-stage only what changed", never
> "build a table with one entry". This fails closed: the operation is
> irreversible and the failure is silent until the peer does not boot.

**The wrong-artifact guard.** `tan flash` must refuse the MRAM-linked peer build,
as `scripts/flash-dualcore.sh` does — the reference calls feeding the wrong one
"the easiest way to waste a bench cycle". With a 256 KiB ITCM budget the two
artifacts differ enormously in size, so the check is cheap and unambiguous.

**Size gate.** The peer's ITCM build is checked against **256 KiB** at plan time
and fails with a real number rather than a link error.

## MRAM reconciliation — RESOLVED

Two shipped layouts, both internally consistent, both filling 5632 KiB exactly:

| layout | reserved | HE | HP | tail |
|---|---|---|---|---|
| Alif DevKit-e8 | — | 2048 KiB @ `0x80000000` | 2048 KiB @ `0x80200000` (+1536 KiB `MRAM_USER` @ `0x80400000`) | — |
| alp-sdk #1069 | 64 | **2688 KiB** @ `0x80010000` | **2688 KiB** @ `0x802b0000` | 64 + 128 |
| dualcore-demo | 64 | **3008 KiB** @ `0x80010000` | **2432 KiB** @ `0x80300000` | 128 |

An image built against one and flashed under the other overlaps: a host image
between 2688 KiB and 3008 KiB is legal under the demo's map and lands on top of
alp-sdk's `hp_slot0`.

### What the DFP fixes, and what it leaves free

Grounded in `alifsemi/alif_ensemble-cmsis-dfp`:

- **MRAM window** — `AlifSemiconductor.Ensemble.pdsc:248`, E8 subFamily:
  `start="0x80000000" size="0x00580000"` (5632 KiB). Both maps respect it.
- **Write granularity — 16 bytes, hard.** `drivers/include/mram.h:23-24`:
  `MRAM_SECTOR_SIZE (0x10)`, `MRAM_ADDR_ALIGN_MASK 0xFFFFFFF0`. Corroborated at
  the tooling level by `app-write-mram`'s "NOT multiple of 16 bytes" warning.
  Both maps use 64 KiB multiples and satisfy it trivially.
- **HE is always low.** Every DevKit-e8 `.alif/*_mram_cfg.json` — Blinky_HE,
  Blinky_HP, Hello_World, DualCore, single-core and combined — puts `HE_APP` at
  `mramAddress 0x80000000` and `HP_APP` at `0x80200000`. HP does not move down
  even when it is the only core. Both our maps already order HE low.
- **The split point is NOT fixed by the vendor.** Alif's linker sizes
  (`APP_MRAM_HE_SIZE`/`APP_MRAM_HP_SIZE` = `0x00200000`, `APP_MRAM_USER_SIZE` =
  `0x00180000`) are CMSIS Configuration-Wizard boilerplate, copy-pasted
  verbatim across AE302/AE402/AE512/AE722/AE822. They are not per-SoC computed
  and validate neither of our splits.

### Decision — adopt alp-sdk's 2688 / 2688 KiB

1. **Symmetric is the only split consistent with role being a `board.yaml`
   choice** (D4). Under 3008/2432 the core you nominate as host silently changes
   how much flash your application gets. Under 2688/2688 it does not.
2. **alp-sdk's split has a recorded rationale** — the ~2.6 MiB NPU MRAM-model
   budget and deferred OTA on both cores. The demo's 3008/2432 has **no recorded
   rationale for the split point**; its docs explain only why HE is low.
3. **The demo's HP MRAM window was never exercised.** Its bench-proven flow
   loads the REMOTE into ITCM (`loadAddress 0x50000000`) and never writes
   `0x300000`, so changing it costs nothing that was ever proven. Its
   `docs/BENCH-DUALCORE.md:332-338` additionally records an unreconciled
   contradiction about which slot map was in effect.
4. **Blast radius is small.** `tan-cli`'s `python/tan/planner/zephyr_board.py`
   (`_aen_role_slot0_map` / `_aen_flash_partitions`) carries **no hardcoded byte
   values**; it reads `base`/`size_kib` from the SoM preset for `mcuboot`,
   `<role>_slot0`, `reserved` and `storage`. Tan renders whatever alp-sdk
   declares, provided those four region names survive — it raises
   `ZephyrBoardEmitError` if any is missing.

The dual-core demo repo changes to match; alp-sdk's `memory_map:` stands.

## Hazard found while grounding the split: `storage` is where the ATOC lands

**This is separate from the split, more dangerous than it, and not yet fixed.**

Alif's tooling does not place the ATOC at a fixed address. `app-gen-toc` /
`app-write-mram` write it immediately **below the top of the MRAM window**,
sized to the generated package — the DFP's own SETOOLS transcript
(`docs/Overview.md:193-224`) shows a 13,552-byte package landing at
**`0x8057cb10`**, against a window top of `0x80580000`.

alp-sdk's map declared a region named **`storage`** at `0x80560000`, 128 KiB,
spanning `0x80560000`–`0x8057FFFF`. **`0x8057cb10` fell inside it.**
RESOLVED in #1289: `storage` now ends at `0x80578000` (96 KiB) and the top
32 KiB is a separate SE-owned `atoc` region, so the landing address above
no longer falls in customer-writable space. The
dual-core demo labels the same window "ATOC application table", which is the
accurate description of what occupies it.

So a region whose name invites treating it as writable user data is where the
boot table actually lives. Writing user storage there corrupts the ATOC;
re-flashing the ATOC destroys the user data. Either direction leaves an
unbootable part, and nothing today prevents either.

Neither repo cites a DFP source for the 128 KiB figure — the actual package
measured 13,552 bytes.

**Actions, before anything ships a two-image project:**

- Rename the region to reflect what it holds, and stop describing it as storage.
- Add a gate asserting no emitted partition table hands that window to an
  application as writable storage.
- Decide whether 128 KiB is the right reservation now that the real package
  size is known, remembering the package grows with the number of ATOC entries
  and a dual-core project has more of them than a single-core one.

## Open questions

## Verification

**Bench-gated** — serial, `e1m-aen-evk-01` under a held labgrid reservation,
AEN Vin **16.0 V**:

1. HE-host / HP-peer end to end from a scaffolded project.
2. HP-host / HE-peer end to end — not a mirror of (1); different ATOC pattern.
3. Resolve the reference repo's own open question on whether the deferred entry
   alone always suffices; two contradictory bench accounts ship there today.
4. Max-size images under the chosen split, both cores flashed together — the
   failure mode is silent overwrite and only appears at the boundary.
5. **Selective flash, then confirm the *other* core still boots** — the test
   that catches a dropped ATOC entry.
6. Cold power cycle, no debugger attached.

**CI-gated:** peer ITCM build fits 256 KiB with RPMsg + OpenAMP + MHU linked;
`tan flash` refuses the MRAM-linked peer (fed the wrong artifact, expects
refusal); emit snapshots pin the `ipc0` node, the ATOC JSON and both cores'
Kconfig sets; `check_core_cmakelists_mapping.py` already covers the two-tree
layout.

Consistent with existing practice: the `aen-dualcore-*` family is `build_only`
in CI and bench-verified separately. CI proves it compiles, links and fits; the
bench proves it runs.

## Open questions

- **TBD-1 — the MRAM split. RESOLVED**: adopt alp-sdk's 2688 / 2688 KiB; the
  dual-core demo repo changes to match. Reasoning in the section above.
- **TBD-4 — the `storage` / ATOC collision.** New, from the DFP grounding pass.
  See the hazard section above. Blocking for any two-image project.
- **TBD-2 — `alif-se-boot` verification status.** That module's README says
  "This has never been run on hardware. UNVERIFIED ON SILICON", while the same
  repo's bench records document dated silicon runs exercising exactly its
  `alif_se_process_toc_entry()`. One is stale. We should not absorb a module
  whose own README disclaims the mechanism we depend on.
- **TBD-3 — partial writes.** Whether `app-write-mram` supports a genuine
  partial write, or whether every flash is necessarily a full-package write. If
  always full-package, "selective" is purely a build-avoidance optimisation and
  the ATOC is always complete — which is safer. Confirm from SETOOLS' own
  documentation.

## Sequencing

| # | step | gate |
|---|---|---|
| 0 | Reconcile the MRAM map across both repos | TBD-1 |
| 1 | Absorb `alif-mhuv2` + `alif-se-boot` under ADR-0017 tiers | TBD-2 |
| 2 | Land `examples/multicore/aen-dualcore-rpmsg` — board.yaml-driven, role-named | — |
| 3 | `board.yaml` `ipc.kind: rpmsg` + `host:`; orchestrator emits the ITCM peer slice | — |
| 4 | `tan flash`: combined ATOC, selective flash, wrong-artifact refusal | TBD-3 |
| 5 | Scaffold emits the two role-named trees | falls out of 2–4 |

## Non-goals

- No sysbuild.
- No core-named directories.
- Not fixing `mproc-mailbox`'s `raw_shmem` example — it stays as the low-level
  `<alp/mproc.h>` demonstration.
- Not vendoring the Alif DFP.
