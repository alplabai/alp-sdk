# SP3 — AEN801 A32-Linux ↔ M55-HP Multicore Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unblock and wire the Linux half of the E1M-AEN801 heterogeneous-IPC contract — ground the Ensemble-E8 memory bases so the rpmsg carveout resolves, add the MHUv2 + remoteproc + reserved-memory DT, retarget the rpmsg example to AEN801, and bake the M55-HP firmware into the image.

**Architecture:** The carveout is currently BLOCKED (e8.json has memory sizes but no bases). Task 1 grounds the bases from the Alif `linux_alif` fork DTS into `e8.json` `memory_regions` (the `n44.json` pattern) → `resolve_carve_outs` resolves. Task 2 retargets the example. Task 3 generates the carveout DTS (`--emit dts-reservations`) + hand-authors the MHUv2/remoteproc nodes (grounded from the fork DTS) into the carrier DTS. Task 4 bakes the M55-HP ELF to the convention firmware path. Task 5 validates the full image. Live A32↔M55 handshake is board-gated.

**Tech Stack:** Python 3.14 (`py -3.14`) for the orchestrator/metadata; Yocto/OE scarthgap + bitbake 2.8 in WSL; Alif `linux_alif` kernel (devkit-e8 fork DTS); Zephyr (M55-HP slice); `<alp/rpc.h>` / `<alp/system_ipc.h>`.

## Global Constraints

- **Never invent pins/addresses/straps/bases** — transcribe from the Alif fork DTS / SoC sources, else mark `TODO(aen-memory-map)` and leave that region blocked. (E8 memory bases are silicon facts present in the fork DTS — grounding them is transcription, not invention.)
- Copyright `Copyright 2026 Alp Lab AB` + `SPDX-License-Identifier: Apache-2.0` (exact "Alp Lab") on new source files; no `Co-Authored-By: Claude` footer.
- C/C++ obeys clang-format-22 (verify with WSL `~/.local/bin/clang-format` 22.1.5 or `py -3.14` pip clang-format 22.1.5 — NOT host clang-format-14). `zephyr/**` is excluded from the gate.
- Metadata edits run the metadata/schema gates with `py -3.14` (default `python` is a stale 3.9).
- Yocto: `MACHINE=e1m-aen801-a32`, `DISTRO=apss-tiny`, `BBMASK = "/meta-alp-sdk/recipes-ros/ /meta-alp-sdk/recipes-deepx/ /meta-alp-sdk/recipes-images/"`, `BB_DANGLINGAPPENDS_WARNONLY = "1"`; write `auto.conf` AFTER the layer-add loop; drive bitbake via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`.
- WSL build tree: `~/alif-bsp-ref/alif_linux-apss-build-setup` (branch `scarthgap_yocto_5.0`); resume per memory `project_e1m_aen_a32_yocto_bringup`.
- Branch `feat/aen-a32-yocto-bringup` (PR #264); commit/push only when the task says.
- Leave AEN701 blocked by design (its `mailbox.controller` is `TBD`); do not touch its blocker.

---

### Task 1: Ground the Ensemble-E8 memory bases into `e8.json` (unblock the carveout)

**Files:**
- Modify: `metadata/socs/alif/ensemble/e8.json` (add a top-level `memory_regions` array)
- Reference: `metadata/socs/renesas/rzv2n/n44.json` (the shape to mirror), `scripts/alp_orchestrate.py::resolve_carve_outs`, `scripts/alp_project.py::resolve_memory_map`

**Interfaces:**
- Produces: `e8.json` `memory_regions` with authoritative bases → `resolve_memory_map` returns them verbatim → `resolve_carve_outs` resolves AEN801 carveouts. Region names MUST include `mram_main` + the shared SRAM banks (`sram0`, `sram1`) since the orchestrator allocates the rpmsg carveout from a shared region.

- [ ] **Step 1: Capture the baseline (carveout BLOCKED) for the 801 board**

Create a throwaway 801 board.yaml + emit reservations:
```
cd C:/Users/caner/Documents/GitHub/alp-sdk
py -3.14 - <<'PY'
b=open('examples/multicore/rpmsg-aen/board.yaml',encoding='utf-8').read().replace('sku: E1M-AEN701','sku: E1M-AEN801')
open('.superpowers/sdd/tmp-board-801.yaml','w',encoding='utf-8',newline='\n').write(b)
PY
py -3.14 scripts/alp_orchestrate.py --input .superpowers/sdd/tmp-board-801.yaml --emit dts-reservations
```
Expected (baseline): a `/* BLOCKED: alp_default_rpmsg -- memory_map.base is unset for region 'sram0' … */` comment. This is the RED state Task 1 must turn GREEN.

- [ ] **Step 2: Ground the E8 base addresses from the Alif fork DTS (WSL)**

The kernel source was cleaned post-build; re-unpack and read the memory nodes. Write `scratchpad/ground_e8_mem.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
SETUP=~/alif-bsp-ref/alif_linux-apss-build-setup
export BITBAKEDIR=$SETUP/tools/bitbake
cd "$SETUP"; source layers/openembedded-core/oe-init-build-env build >/dev/null
bitbake -c unpack linux-alif
W=$(find tmp/work/e1m_aen801_a32-poky-linux-musleabi/linux-alif -maxdepth 2 -type d -name git | head -1)
echo "SRC=$W"
echo "=== memory@ / reserved-memory / mram / sram nodes in the ensemble DTS tree ==="
grep -rniE "memory@|reserved-memory|mram|sram|itcm|dtcm|reg = <0x" \
  "$W/arch/arm/boot/dts/alif/ensemble" 2>/dev/null | grep -iE "memory|mram|sram|tcm|reg" | head -60
```
Run: `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/Users/caner/AppData/Local/Temp/claude/C--Users-caner-Documents-GitHub-alp-sdk/4b876532-1fb0-4dc5-81e1-0d7f1581fd9a/scratchpad/ground_e8_mem.sh`
Record the authoritative base addresses for MRAM and the SRAM banks. If the fork DTS does not expose a base for a region, mark it `TODO(aen-memory-map)` and leave that region out (it stays blocked) — **do not invent**.

- [ ] **Step 3: Add the `memory_regions` block to `e8.json`**

Match the `n44.json` shape (keys: `name`, `base`, `size_kib` or `size_mib`, `accessible_from`, `cacheable`). Use the grounded bases from Step 2 and the sizes already in the variant (`mram_mb: 5.5`, `sram_banks_kb`). `accessible_from` lists the SoC `cores[]` ids for shared regions; TCM banks list only their owning core. Example shape (BASES from Step 2 — the `0x…` values below are placeholders to be replaced with grounded addresses, NOT invented):
```json
"memory_regions": [
  { "name": "mram_main", "base": <grounded>, "size_kib": 5632, "accessible_from": ["a32_cluster","m55_hp","m55_he"], "cacheable": true },
  { "name": "sram0", "base": <grounded>, "size_kib": 4096, "accessible_from": ["a32_cluster","m55_hp","m55_he"], "cacheable": true },
  { "name": "sram1", "base": <grounded>, "size_kib": 4096, "accessible_from": ["a32_cluster","m55_hp","m55_he"], "cacheable": true },
  { "name": "sram2_m55_hp_itcm", "base": <grounded>, "size_kib": 256, "accessible_from": ["m55_hp"], "cacheable": false },
  { "name": "sram3_m55_hp_dtcm", "base": <grounded>, "size_kib": 1024, "accessible_from": ["m55_hp"], "cacheable": false }
]
```
Confirm the SoC `cores[]` ids (`py -3.14 -c "import json;print([c['id'] for c in json.load(open('metadata/socs/alif/ensemble/e8.json'))['cores']])"`) and use those exact ids in `accessible_from`.

- [ ] **Step 4: Verify the carveout now RESOLVES + no E8 consumer regresses**

```
py -3.14 scripts/alp_orchestrate.py --input .superpowers/sdd/tmp-board-801.yaml --emit dts-reservations
```
Expected (GREEN): a real `alp_default_rpmsg: alp_default_rpmsg@<base> { compatible = "shared-dma-pool"; reg = <…>; no-map; … };` node — NO `BLOCKED` comment.

Run the metadata/schema + doc gates:
```
py -3.14 scripts/check_doc_drift.py
ls tests/scripts/ | grep -iE "soc|metadata|memory" && py -3.14 -m pytest tests/scripts -k "soc or metadata or memory" -q
```
Expected: gates pass. If any existing E8 board.yaml projection changed, confirm the new `memory_regions` covers the regions it needs (the regions list is now verbatim, not size-derived).

- [ ] **Step 5: Commit**

```bash
git add metadata/socs/alif/ensemble/e8.json
git commit -m "metadata(aen): ground Ensemble-E8 memory_regions bases (unblocks AEN801 IPC carveout)"
rm -f .superpowers/sdd/tmp-board-801.yaml
```

---

### Task 2: Retarget `examples/multicore/rpmsg-aen/` to AEN801 + dual-SKU

**Files:**
- Rename: `examples/multicore/rpmsg-aen/board.yaml` → keep `board.yaml` as the **AEN801 default** (the M55-HP CMakeLists hardcodes `../board.yaml`), and add `examples/multicore/rpmsg-aen/board-aen701.yaml` preserving the old 701 content.
- Modify: `examples/multicore/rpmsg-aen/README.md` (dual-SKU build flow)

**Interfaces:**
- Consumes: Task 1 (`e8.json` bases) so the 801 carveout resolves.
- Produces: `board.yaml` with `som.sku: E1M-AEN801`; `board-aen701.yaml` with `som.sku: E1M-AEN701`. Both share `preset: e1m-evk`, the same `cores:`/`ipc:` blocks, and the shared `linux/` + `m55_hp/` sources.

> **Deviation from spec naming (intentional):** the spec named the buildable file `board-aen801.yaml`, but `m55_hp/CMakeLists.txt` + `linux/CMakeLists.txt` hardcode `../board.yaml`. Making `board.yaml` itself the AEN801 default (and `board-aen701.yaml` the alternate) keeps those builds working with no copy to drift. The README documents the mapping.

- [ ] **Step 1: Preserve the 701 config as the alternate**

```
cd C:/Users/caner/Documents/GitHub/alp-sdk
git mv examples/multicore/rpmsg-aen/board.yaml examples/multicore/rpmsg-aen/board-aen701.yaml
```

- [ ] **Step 2: Create the AEN801 `board.yaml` (the default)**

Copy `board-aen701.yaml` to `board.yaml`, change `som.sku: E1M-AEN701` → `som.sku: E1M-AEN801`, drop the `hw_rev: r1` line if AEN801 has no rev yet (confirm against `metadata/e1m_modules/E1M-AEN801.yaml`), and rewrite the header comment block: this is the AEN801 (E8) default; AEN701 is the alternate in `board-aen701.yaml`, still blocked on its `mailbox.controller: TBD`. Keep `preset: e1m-evk`, the `pins:`, `cores:` (a32_cluster `./linux`, m55_hp `./m55_hp`, m55_he off), and `ipc:` (rpmsg 256 KB `alp_default_rpmsg` cacheable) identical.

- [ ] **Step 3: Validate both board files**

```
py -3.14 scripts/alp_project.py --input examples/multicore/rpmsg-aen/board.yaml --emit zephyr-conf --core m55_hp >/dev/null && echo "801 OK"
py -3.14 scripts/alp_orchestrate.py --input examples/multicore/rpmsg-aen/board.yaml --emit dts-reservations | grep -q "shared-dma-pool" && echo "801 carveout RESOLVES"
py -3.14 scripts/alp_orchestrate.py --input examples/multicore/rpmsg-aen/board-aen701.yaml --emit dts-reservations | grep -qi "BLOCKED" && echo "701 still blocked (expected)"
```
Expected: `801 OK`, `801 carveout RESOLVES`, `701 still blocked (expected)`.

- [ ] **Step 4: Update the README for the dual-SKU flow**

Document: default build = AEN801 (`board.yaml`); the 701 alternate = `--input board-aen701.yaml` (blocked until its mailbox lands). Note the shared sources + the EVK-common sensors. Add a one-line known-issue: the producer source comment references `LSM6DSO`, but the `e1m-evk` preset populates `bmi323` — a pre-existing draft mismatch, board-gated, out of SP3 scope (flagged for a follow-up).

- [ ] **Step 5: Commit**

```bash
git add examples/multicore/rpmsg-aen/
git commit -m "examples(aen): retarget rpmsg-aen to AEN801 default + keep AEN701 alternate"
```

---

### Task 3: Carrier DTS — carveout (generated) + MHUv2 + remoteproc(M55-HP)

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif/files/aen801-dts-reservations.dtsi` (the generated carveout, checked in)
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` (add `#include` + MHUv2 + remoteproc nodes)
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend` (ship the new `.dtsi` into the kernel source so the `#include` resolves)

**Interfaces:**
- Consumes: Task 1 (`e8.json` bases) + Task 2 (`board.yaml` 801). The generated `.dtsi` defines `reserved-memory { alp_default_rpmsg: …@<base> { … }; }`.
- Produces: a carrier dtb whose decompile shows `reserved-memory`, an enabled MHUv2 mailbox node, and a `remoteproc` node for M55-HP referencing the carveout + firmware `alp/E1M-AEN801/m55_hp.elf`.

- [ ] **Step 1: Generate + check in the carveout reservations dtsi**

```
cd C:/Users/caner/Documents/GitHub/alp-sdk
py -3.14 scripts/alp_orchestrate.py --input examples/multicore/rpmsg-aen/board.yaml \
  --emit dts-reservations \
  > meta-alp-sdk/recipes-kernel/linux/linux-alif/files/aen801-dts-reservations.dtsi
head -20 meta-alp-sdk/recipes-kernel/linux/linux-alif/files/aen801-dts-reservations.dtsi
```
Expected: the file starts with the "Auto-generated … do not edit" banner and contains an `alp_default_rpmsg@<base>` node (no BLOCKED comment).

- [ ] **Step 2: Ground the MHUv2 + (any) remoteproc nodes from the fork DTS (WSL)**

Reusing the unpacked tree from Task 1 Step 2, write `scratchpad/ground_e8_mhu.sh` grepping the ensemble DTS for the mailbox + remoteproc:
```bash
#!/usr/bin/env bash
set -euo pipefail
SETUP=~/alif-bsp-ref/alif_linux-apss-build-setup; export BITBAKEDIR=$SETUP/tools/bitbake
cd "$SETUP"; source layers/openembedded-core/oe-init-build-env build >/dev/null
W=$(find tmp/work/e1m_aen801_a32-poky-linux-musleabi/linux-alif -maxdepth 2 -type d -name git | head -1)
grep -rniE "mhuv2|mhu@|remoteproc|rproc|mboxes|alif,mhu" "$W/arch/arm/boot/dts/alif" 2>/dev/null | head -50
```
Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/ground_e8_mhu.sh`. Record the MHUv2 node's `compatible`, `reg`, `interrupts` and any existing remoteproc node. If the fork DTS already wires M55 remoteproc for Linux, mirror it verbatim (rename label only). If it has the MHU node but no Linux remoteproc, author the remoteproc node from these grounded values + the `E1M-AEN801.yaml` channel map; mark any genuinely-absent reg/IRQ `TODO(aen-memory-map)`.

- [ ] **Step 3: Edit the carrier DTS — include the carveout + add MHU/remoteproc**

Append to `e1m-aen801-evk.dts` (after the existing `&pinmux` block). The `<…>` values are the Step-2 grounded values — do NOT invent; leave `TODO(aen-memory-map)` for any the fork omits:
```dts
/* SP3 multicore: rpmsg over MHUv2 to M55-HP. */
#include "aen801-dts-reservations.dtsi"   /* generated reserved-memory carveout */

&<mhu_node_label> {                        /* grounded label from the fork DTS */
	status = "okay";
};

/ {
	remoteproc_m55_hp: remoteproc-m55-hp {
		compatible = <grounded-or-TODO(aen-memory-map)>;
		mboxes = <&<mhu_node_label> 0>;    /* ch0 = alp_default_rpmsg per E1M-AEN801.yaml */
		memory-region = <&alp_default_rpmsg>;
		firmware-name = "alp/E1M-AEN801/m55_hp.elf";
		status = "okay";
	};
};
```
Match the carrier DTS's existing tab/brace style.

- [ ] **Step 4: Ship the dtsi into the kernel build (bbappend)**

In `linux-alif_%.bbappend`, add `file://aen801-dts-reservations.dtsi` to `SRC_URI` and install it alongside `e1m-aen801-evk.dts` in the kernel's `arch/arm/boot/dts/alif/.../e1m/` dir (mirror how SP1's bbappend installs the `.dts`). Read the current bbappend first and follow its existing install pattern exactly.

- [ ] **Step 5: Compile the dtb + decompile-verify (WSL)**

Write `scratchpad/build_sp3_dtb.sh` (source env, `bitbake -c compile linux-alif`, then `dtc` decompile the produced `e1m-aen801-evk.dtb`), run it, and grep the decompile:
```
... | grep -iE "reserved-memory|alp_default_rpmsg|remoteproc|mhu"
```
Expected: the reserved-memory carveout, the remoteproc node, and the enabled MHU node all present. Quote the relevant decompile lines.

- [ ] **Step 6: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/files/aen801-dts-reservations.dtsi \
        meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts \
        meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend
git commit -m "dts(aen): MHUv2 + remoteproc(M55-HP) + generated rpmsg carveout in the carrier DTS"
```

---

### Task 4: Bake the M55-HP firmware to the convention path

**Files:**
- Create: `meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/aen-m55-hp-fw_0.6.bb`
- Reference: `docs/heterogeneous-builds.md` (firmware path convention), `examples/multicore/rpmsg-aen/m55_hp/` (the slice), `meta-alp-sdk/recipes-core/alp-system/alp-remoteproc_0.6.bb` (the launcher that scans `/lib/firmware`)

**Interfaces:**
- Consumes: Tasks 1-3. Produces a package installing the M55-HP ELF to `/lib/firmware/alp/E1M-AEN801/m55_hp.elf` (the path the `remoteproc` node's `firmware-name` and `alp-remoteproc-start.sh` expect).

- [ ] **Step 1: Build the M55-HP ELF via west (the slice's own toolchain)**

The M55-HP slice is a Zephyr app; build it with the documented heterogeneous flow (NOT inside bitbake — Zephyr toolchain is not in the OE sysroot; spec risk #2). Write `scratchpad/build_m55_hp.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk
export ALP_SDK_ROOT=$PWD
west build -b alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp \
  examples/multicore/rpmsg-aen/m55_hp -d /tmp/m55_hp_build 2>&1 | tail -30
file /tmp/m55_hp_build/zephyr/zephyr.elf
cp /tmp/m55_hp_build/zephyr/zephyr.elf \
  meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/files/m55_hp.elf
```
Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/build_m55_hp.sh`.
Expected: a Cortex-M55 ELF. **If the west build fails** (missing Zephyr/board deps in WSL), STOP and report BLOCKED with the error — do not fake an ELF; the controller decides whether to fix the Zephyr env or defer Task 4's bake to bench while still landing the recipe + DT.

- [ ] **Step 2: Write the prebuilt-artifact recipe**

`aen-m55-hp-fw_0.6.bb` — installs the checked-in `files/m55_hp.elf` to the convention path. Model the boilerplate on a `recipes-core/alp-system/*` file recipe (uses `${COMMON_LICENSE_DIR}/Apache-2.0`):
```bitbake
# SPDX-License-Identifier: Apache-2.0
SUMMARY = "E1M-AEN801 M55-HP rpmsg producer firmware (remoteproc)"
DESCRIPTION = "Cortex-M55-HP Zephyr firmware for the rpmsg-aen demo; \
               installed where alp-remoteproc scans so the A32 Linux \
               side can boot it over MHUv2."
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

SRC_URI = "file://m55_hp.elf"
S = "${WORKDIR}"

do_install() {
    install -d ${D}/lib/firmware/alp/E1M-AEN801
    install -m 0644 ${WORKDIR}/m55_hp.elf \
        ${D}/lib/firmware/alp/E1M-AEN801/m55_hp.elf
}

FILES:${PN} = "/lib/firmware/alp/E1M-AEN801/m55_hp.elf"
COMPATIBLE_MACHINE = "e1m-aen801-a32"
# Prebuilt: the ELF is built out-of-tree via `west build` (see the
# example README) since the Zephyr toolchain is not in the OE sysroot.
```

- [ ] **Step 3: Recipe-parse check (WSL)**

```
... bitbake -e aen-m55-hp-fw | grep -E "^FILES|^COMPATIBLE_MACHINE" | head
```
Expected: parses; FILES shows the firmware path. (Full bake is Task 5.)

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/
git commit -m "yocto(aen): bake M55-HP rpmsg firmware to /lib/firmware/alp/E1M-AEN801"
```

---

### Task 5: Image install + full-bake validation + docs

**Files:**
- Modify: `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf` (IMAGE_INSTALL += firmware + alp-remoteproc)
- Modify: `CHANGELOG.md`, `examples/multicore/README.md` (note the AEN801 retarget), `docs/heterogeneous-builds.md` (if it lists per-SKU status)

**Interfaces:**
- Consumes: Tasks 1-4. Produces a baked `alif-tiny-image` whose rootfs manifest carries the M55-HP firmware + `alp-remoteproc`, and whose dtb carries the carveout/remoteproc nodes.

- [ ] **Step 1: Add the image installs**

In `e1m-aen801-a32.conf`, extend the existing `IMAGE_INSTALL:append` line (from SP2: `" alp-sdk aen-a32-carrier-bringup"`) to add ` aen-m55-hp-fw alp-remoteproc`. Read the file first; edit only that line.

- [ ] **Step 2: Full image bake (WSL)**

Push the branch first (the example recipes fetch `${AUTOREV}`): `git push`. Then write `scratchpad/build_sp3_image.sh` (source env, `bitbake -c cleansstate aen-m55-hp-fw alp-remoteproc`, `bitbake alif-tiny-image`, tee to a log in scratchpad), run it.
Expected: bake completes, no ERROR lines. If it fails on a blocked carveout `#error` from `ipc-contract-h`, Task 1 didn't fully resolve — return to Task 1.

- [ ] **Step 3: Assert firmware + nodes landed**

```
# firmware in the rootfs manifest:
... grep -E "aen-m55-hp-fw|alp-remoteproc" tmp/deploy/images/e1m-aen801-a32/*.manifest
# carveout/remoteproc in the deployed dtb (decompile):
... dtc -I dtb -O dts tmp/deploy/images/e1m-aen801-a32/e1m-aen801-evk.dtb | grep -iE "reserved-memory|alp_default_rpmsg|remoteproc|mhu"
```
Expected: the firmware/remoteproc packages in the manifest; the carveout + remoteproc + MHU nodes in the dtb.

- [ ] **Step 4: Record evidence + CHANGELOG + example index**

Append a `## SP3 bake` section to `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` (bake result, manifest line, dtb node lines). Add a CHANGELOG entry (AEN801 multicore: e8.json bases grounded → carveout resolves; MHUv2 + remoteproc DT; M55-HP firmware baked). Add the AEN801 retarget note to `examples/multicore/README.md`. Run `py -3.14 scripts/check_doc_drift.py` (expect OK).

- [ ] **Step 5: Commit**

```bash
git add meta-alp-sdk/conf/machine/e1m-aen801-a32.conf CHANGELOG.md \
        examples/multicore/README.md docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md
git commit -m "yocto(aen): bake M55-HP firmware + remoteproc into alif-tiny-image (SP3)"
```

---

## Self-Review

**Spec coverage:**
- Component 0 (e8.json bases) → Task 1. ✅
- Component 1 (dual-SKU retarget) → Task 2. ✅ (deviation: `board.yaml`=801 default + `board-aen701.yaml`, documented + justified by the hardcoded CMakeLists path.)
- Component 2 (carrier DTS carveout + MHU + remoteproc) → Task 3 (generated `.dtsi` `#include` + grounded MHU/remoteproc). ✅
- Component 3 (M55-HP firmware bake) → Task 4 (prebuilt-artifact recipe + west build; BLOCKED-report fallback per spec risk #2). ✅
- Component 4 / Validation (image install + bake + manifest/dtb assert) → Task 5. ✅
- Validation step 2 (carveout resolves vs 701 blocked) → Task 2 Step 3. ✅
- Board-gated (absent bases/MHU reg → TODO; live handshake) → Task 1 Step 2 / Task 3 Step 2-3 markers. ✅

**Placeholder scan:** The `<grounded>` / `<mhu_node_label>` / `TODO(aen-memory-map)` markers are deliberate — the plan forbids inventing addresses (Global Constraints) and the values are transcribed at implementation time from the fork DTS (the SP1 method). They are confined to the two grounding steps (Task 1 Step 2, Task 3 Step 2) with the exact command that produces them. No vague "add error handling"-style gaps.

**Type/name consistency:** firmware path `/lib/firmware/alp/E1M-AEN801/m55_hp.elf` identical across Task 3 (`firmware-name`), Task 4 (recipe `do_install` + FILES), and the `alp-remoteproc` scan. Carveout label `alp_default_rpmsg` consistent (board.yaml `ipc.name` → generated node label → DT `memory-region` phandle). Recipe name `aen-m55-hp-fw` consistent across Task 4 + Task 5 IMAGE_INSTALL + manifest grep. Board files: `board.yaml` (801) + `board-aen701.yaml` consistent across Tasks 2-5.
