# SP3 — E1M-AEN801 A32-Linux ↔ M55-HP multicore (rpmsg over MHUv2)

**Date:** 2026-06-26
**Branch:** `feat/aen-a32-yocto-bringup` · **PR:** #264
**Predecessors:** SP1 (`2026-06-25-aen-a32-yocto-bringup-design.md` — carrier dtb +
`alif-tiny-image`), SP2 (`2026-06-25-aen-a32-sp2-peripheral-runtime-design.md` —
A32 `alp_*` peripheral example baked into the image).

## Goal

Stand up the **Linux half of the heterogeneous-IPC contract** for `E1M-AEN801`:
the booted A32 Linux releases the **M55-HP** core (remoteproc) and an `rpmsg`
channel comes up over the **ARM MHUv2** mailbox, carrying the
`alp_default_rpmsg` (ch0) endpoint between an A32 `<alp/rpc.h>` consumer and a
Zephyr producer on the M55-HP. Everything buildable now is built and baked into
the image; only the live A32↔M55 handshake is board-gated.

This parallels the V2N A55+M33 remoteproc/rpmsg pattern. The `alp-remoteproc`
systemd launcher already targets `e1m-aen.*` — SP3 supplies the DT carveout +
mailbox + remoteproc nodes and the M55-HP firmware that launcher needs.

## Grounding (authoritative, nothing invented)

| Fact | Source |
|---|---|
| Mailbox = ARM **MHUv2**, controller token `alif_mhuv2`, compatible `alif,mhuv2-mbox`; ch0=`alp_default_rpmsg`, ch1/2=app, ch3=power_mgmt | `metadata/e1m_modules/E1M-AEN801.yaml` `mailbox:` (RESOLVED, not TBD) |
| AEN801 memory **sizes**: MRAM 5.5 MB, SRAM0/1 4 MB each, M55-HP ITCM 256 KB / DTCM 1024 KB | `metadata/socs/alif/ensemble/e8.json` variant `AE822FA0E5597LS0` (`alp_module_skus: [E1M-AEN801]`) |
| **Carveout is BLOCKED today**: e8.json has **no `memory_regions`** (bases), so `resolve_carve_outs` cannot place the region — verified `--emit dts-reservations` emits `/* BLOCKED: alp_default_rpmsg -- memory_map.base is unset for region 'sram0' … */` | `scripts/alp_orchestrate.py::resolve_carve_outs` / `alp_project.py::resolve_memory_map` |
| The fix pattern: a SoC-JSON `memory_regions` block carrying authoritative bases (e.g. V2N `ddr_main` base `0x48000000`, `m33_tcm` `0x80000000`) makes `resolve_memory_map` return them verbatim → carveout resolves | `metadata/socs/renesas/rzv2n/n44.json` (the precedent) |
| `alp-remoteproc` systemd unit (walks `/sys/class/remoteproc/`, starts ALP M-class fw, waits for `/dev/rpmsg_ctrl0`) already `COMPATIBLE_MACHINE = "…|e1m-aen.*|…"` | `meta-alp-sdk/recipes-core/alp-system/alp-remoteproc_0.6.bb` |
| Firmware path convention `/lib/firmware/alp/<SKU>/<core>.elf` | `docs/heterogeneous-builds.md` |
| EVK sensors (bmi323 @0x68, bmp581) are EVK-carrier-common across AEN701/801 | `metadata/boards/e1m-evk.yaml`; both SKUs use `preset: e1m-evk` |

**The build-readiness asymmetry:** `E1M-AEN801` has a **resolved** mailbox
(`alif_mhuv2`) and real memory **sizes** — it is one authoritative-base block
(SoC `memory_regions` in e8.json) away from a resolvable carveout. `E1M-AEN701`
is blocked on **both** its `mailbox.controller: TBD` *and* the same missing
bases. So SP3 unblocks 801 by grounding the E8 bases (Component 0); 701 stays
blocked on its mailbox and is out of scope. The existing
`examples/multicore/rpmsg-aen/` targets 701 only and is `[UNTESTED]`.

## Non-goals

- Live rpmsg handshake / remoteproc start on silicon (board-gated).
- M55-HE participation (stays at the SoM stock-shim default, per the contract).
- Unblocking AEN701 (needs its own `mailbox.controller` HW-config; documented, not done).
- Changing the orchestrator (`alp_project.py` / `alp_orchestrate.py`) — SP3 consumes it as-is.

## Components

### 0. Ground the Ensemble-E8 memory bases into `e8.json` (unblocks the carveout)

`metadata/socs/alif/ensemble/e8.json` gains a top-level `memory_regions` block
(the `n44.json` shape: `name`, `base`, `size_kib`/`size_mib`, `accessible_from`,
`cacheable`) carrying the **authoritative Ensemble-E8 base addresses** for the
regions an IPC carveout can land in (at minimum `mram_main` + the shared SRAM
banks; M55-HP TCM optional). Bases are **transcribed from the Alif `linux_alif`
devkit-e8 fork DTS `memory@`/`reserved-memory` nodes** (re-`bitbake -c unpack
linux-alif`, the SP1 grounding method) — silicon facts, zero-invention. Any base
genuinely absent from the fork stays `TBD` (and its region stays blocked).

`resolve_memory_map` prefers `soc_memory_regions` verbatim when present, so this
makes `--emit dts-reservations` for `board-aen801.yaml` produce a **real
reserved-memory node** instead of the `/* BLOCKED … */` comment.

**Blast radius:** e8.json is shared SoC metadata. Adding `memory_regions` flips
`resolve_memory_map` from size-derived (sram_banks) to the verbatim region list
for *every* E8 consumer — so the block must cover the regions existing E8
board.yamls rely on. The task verifies no existing E8 projection regresses
(re-run any E8 `--emit` + the metadata/schema gates).

### 1. Retarget `examples/multicore/rpmsg-aen/` to cover both SKUs

Shared, SKU-neutral sources (kept as-is): `linux/src/main.c` (A32 `<alp/rpc.h>`
consumer) + `m55_hp/src/main.c` (Zephyr producer reading the EVK bmi323/bmp581
and publishing over `alp_default_rpmsg`). The `e1m-evk` preset + its sensors are
common to both SKUs, so no source fork.

Per-SKU board configs (the orchestrator takes `--input <path>`):
- `board-aen701.yaml` — the existing `board.yaml` renamed, `sku: E1M-AEN701`
  (documented as blocked until its mailbox lands).
- **`board-aen801.yaml`** (new) — `sku: E1M-AEN801`, same `preset: e1m-evk`, same
  `cores:` (a32_cluster `app: ./linux`, m55_hp `app: ./m55_hp`, m55_he off) and
  same `ipc:` (rpmsg, `carve_out_kb: 256`, `name: alp_default_rpmsg`,
  `cacheable: true`).
- Keep a `board.yaml` that defaults to the buildable SKU (copy of
  `board-aen801.yaml`) so bare `--input board.yaml` works; the README documents
  `--input board-aen<sku>.yaml` for either SKU.

The existing `board.yaml` header comment (701-specific, "memory_map TBD") is
updated to describe the dual-SKU layout + the 801-is-buildable / 701-is-blocked
split.

### 2. Carrier DTS multicore nodes

`meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` (the SP1
carrier DTS) gains, grounded from the Alif `linux_alif` devkit-e8 fork DTS
(re-`bitbake -c unpack linux-alif`, the SP1 method) + the e8.json memory-map:

- the `reserved-memory` carveout for the rpmsg vrings + M55-HP firmware load
  region. **Prefer the orchestrator-generated `dts-reservations.dtsi`** (`--emit
  dts-reservations` → check the generated file into the bbappend's
  `SRC_URI`/`files/` and `#include` it from the carrier DTS) so base+size stay the
  single derived projection of Component 0 + `carve_out_kb` (256 KB) — no
  hand-copied addresses. (Regenerate when board.yaml `ipc:` or e8.json changes.)
- the MHUv2 mailbox node(s) enabled (`compatible = "alif,mhuv2-mbox"` /
  whatever the fork DTS uses), reg/IRQ from the fork DTS (else `TODO`).
- a `remoteproc` node for M55-HP: `mboxes` = the MHUv2 channel(s) for ch0
  `alp_default_rpmsg`, `memory-region` = the carveout phandle, `firmware` =
  `alp/E1M-AEN801/m55_hp.elf`.

If the devkit-e8 fork DTS already wires M55 remoteproc/mailbox/reserved-memory
for Linux, mirror it verbatim (rename labels only). Override `*_STATUS`-style
guards after the common include, as SP1 did.

**Consistency constraint:** using the generated `dts-reservations.dtsi` makes the
DT carveout base+size the single derived projection of Component 0 (e8.json bases)
+ `board-aen801.yaml` `carve_out_kb` — no hand-copied addresses to drift. The MHU
+ remoteproc nodes are the only hand-authored multicore DT (grounded from the
fork DTS).

### 3. M55-HP firmware bake

A recipe builds the `m55_hp/` Zephyr producer for the AEN801 M55-HP Zephyr board
(`alp_e1m_aen801_m55_hp`) and installs the ELF to
`/lib/firmware/alp/E1M-AEN801/m55_hp.elf` (the `docs/heterogeneous-builds.md`
convention the `alp-remoteproc` launcher scans for). Modeled on the existing
firmware-bake recipes if one exists for V2N's M33; otherwise a new
`meta-alp-sdk/recipes-firmware/` recipe that invokes the Zephyr/west build of
the slice and installs the artifact.

`alp-remoteproc` already autostarts on `e1m-aen.*` — no recipe change there;
SP3 only adds the firmware package + `IMAGE_INSTALL`s it (and `alp-remoteproc`)
in `e1m-aen801-a32.conf`, plus optionally the A32 `rpmsg_aen_consumer` binary
(SP2's example-bake pattern).

### 4. Validation (no-board)

1. **dtb compiles** + decompile shows the `reserved-memory`, MHUv2 mailbox, and
   `remoteproc` (M55-HP) nodes — SP1 method (`bitbake -c compile linux-alif` +
   `dtc` decompile), in the standing WSL build tree.
2. **Orchestrator carveout resolve:** after Component 0,
   `py -3.14 scripts/alp_orchestrate.py --input
   examples/multicore/rpmsg-aen/board-aen801.yaml --emit dts-reservations` emits a
   **real `reserved-memory` node** (a `…@<base>` node with `compatible =
   "shared-dma-pool"`, not the `/* BLOCKED … */` comment) — proving Component 0
   unblocked the 801 carveout. (Baseline: the same command before Component 0, and
   for 701, emits the BLOCKED comment.)
3. **Image bake:** rebuild the image with the M55-HP firmware package +
   `alp-remoteproc`; assert `/lib/firmware/alp/E1M-AEN801/m55_hp.elf` and the
   firmware package appear in the rootfs manifest and the carveout/remoteproc
   nodes are in the deployed dtb (SP2 method).

## Board-gated (deferred to bench, `TODO(aen-memory-map)` where absent)

- Any E8 base genuinely absent from the fork DTS (region stays blocked) + MHU
  reg/IRQ if the fork omits them.
- The live `remoteproc start` of M55-HP and the rpmsg channel handshake
  (`/dev/rpmsg_ctrl0` + the consumer receiving the producer's messages).
- AEN701's `mailbox.controller` HW-config (separate follow-up; SP3 leaves 701
  blocked by design).

## Build / environment constraints

- `MACHINE=e1m-aen801-a32`, `DISTRO=apss-tiny`, the SP1 `BBMASK` +
  `BB_DANGLINGAPPENDS_WARNONLY=1`; `auto.conf` after the layer-add loop; drive
  bitbake via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`.
- New C/C++ obeys the alp-sdk clang-format-22 house style (verify with
  `~/.local/bin/clang-format` 22.1.5 in WSL or `py -3.14` pip clang-format
  22.1.5 — NOT host clang-format-14). Note `zephyr/**` is excluded from the gate;
  the M55 producer source under `m55_hp/` follows the example's existing style.
- No invented pins/addresses/straps/bases — transcribe or `TODO(aen-memory-map)`.
- "Alp Lab" spelling; no `Co-Authored-By: Claude` footer.

## Risks

- **Fork DTS may not wire M55-for-Linux remoteproc.** If devkit-e8's Linux DTS
  has no M55 remoteproc/reserved-memory (Alif may ship M55 firmware via SE/ATOC,
  not Linux remoteproc), the nodes are authored from the MHUv2 spec + e8.json
  with `TODO(aen-memory-map)` bases, and the dtb still compiles — but the
  remoteproc node's correctness is bench-gated. Surface this in the bake notes;
  do not invent a base.
- **Firmware-bake recipe may need a Zephyr SDK in the Yocto build.** If baking
  the M55 ELF inside bitbake is impractical (Zephyr toolchain not in the OE
  sysroot), fall back to a prebuilt-artifact recipe that consumes a
  west-built ELF, and document the two-step build — keep the image-bake green.
- **701 stays blocked.** Explicitly documented; not a regression — the retarget
  makes 801 buildable without touching 701's blocker.
