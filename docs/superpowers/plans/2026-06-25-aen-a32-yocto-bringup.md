# E1M-AEN801 A32 Yocto/Linux Bring-up (SP1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the Alif Ensemble E8 Cortex-A32 Linux image for `E1M-AEN801` on the `e1m-evk` carrier as far as is buildable without the physical board.

**Architecture:** Ride OVER the Alif vendor BSP (`meta-alif-ensemble@scarthgap`, `devkit-e8`) per ADR 0017 — consume `linux-alif`/TF-A/SoC-dtsi/`xipImage` as Tier-1; author only the value-add board layer (E1M SoM dtsi + E1M-EVK carrier dtsi + product dts + `linux-alif` bbappend + machine-conf overrides + kernel `.cfg` fragment + CI row). AEN A32 is *just another MACHINE* in the unified `meta-alp-sdk` layer — V2N-A55 parity, no parallel AEN stack. HW-sensitive resolved values (TF-A map, HyperRAM timings) live in `alp-sdk-internal`; the public tree carries sanitized skeletons + `TBD(alif-hw-config)`.

**Tech Stack:** Yocto/BitBake (scarthgap), `meta-alif-ensemble`, `linux-alif`, device tree (`dtc`), `meta-alp-sdk`, alp-sdk metadata generators (`scripts/gen_*.py`), WSL Ubuntu build tree.

**Spec:** `docs/superpowers/specs/2026-06-25-aen-a32-yocto-bringup-design.md`

## Global Constraints

- **No invented HW values.** Every carrier-specific value is sourced from the real `devkit-e8` base / the SoM preset, or marked `TBD(alif-hw-config)`. Never fabricate addresses, timings, pins, or platform strings.
- **Ride over, don't fork** `meta-alif-ensemble` (ADR 0017 Tier-1). Consume `require conf/machine/devkit-e8.conf`; author only the board overlay.
- **Unification:** AEN A32 reuses V2N machinery wherever SoM-agnostic. Mirror the V2N reference files (`e1m-v2n-som.dtsi`, `e1m-x-evk.dtsi`, `e1m-v2n101-x-evk.dts`, `linux-renesas_%.bbappend`, `e1m-v2n101-a55.conf`). No AEN-special-casing without a recorded reason.
- **Vendor-clean portable surface:** no `alif_*`/`ALIF_*` tokens in `include/alp/**` or `src/common/**`.
- **One source of truth:** HW facts live in `metadata/**`; generated files (`soc_caps.h`, route headers) are regenerated, never hand-edited. DTS is the one hand-authored artifact and must reference (not contradict) the metadata.
- **Public/internal split:** resolved TF-A memory map + HyperRAM timings + schematic-derived values → `alp-sdk-internal`; public gets sanitized + TBD.
- **Branch:** `feat/aen-a32-yocto-bringup` (off `dev`). Commits: bare `git commit -q -m`, no Claude footer, staged separately.
- **Yocto series:** `scarthgap` (matches `conf/layer.conf LAYERSERIES_COMPAT`).
- **Memory model:** XIP Linux — OSPI0 CS0 = MX25UM25645 OctaFlash (32 MiB NOR, ROM/boot); OSPI0 CS1 = W958D8NBYA5I OctalRAM (32 MiB HyperRAM, RAM); on-die ~9984 KiB SRAM + 5.5 MiB MRAM. `xipImage` kernel from NOR. RAM/ROM population is a per-board parameter (default: NOR=ROM, HyperRAM=RAM).

---

## File Structure

**Author (public `alp-sdk`):**
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-som.dtsi` — on-module nodes (OSPI, CC3501E, reserved-mem, MHUv2, regulators)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-evk.dtsi` — carrier nodes (console, I²C, GPIO expander, aliases, bootargs)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` — product (composes SoC+SoM+carrier)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend` — feed the DTS into `linux-alif`
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.cfg` — kernel defconfig fragment (peripherals not in devkit defconfig)
- `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf` — MODIFY (fill KERNEL_DEVICETREE, boot, TF-A TBD)
- `meta-alp-sdk/recipes-images/alp-image-edge.bb` — MODIFY (machine-conditioned XIP/size knobs)
- `.github/workflows/pr-bitbake.yml` — MODIFY (add `e1m-aen801-a32` matrix row)
- `docs/bring-up-aen.md`, `docs/os-support-matrix.md`, `CHANGELOG.md` — MODIFY (docs)
- `metadata/**` — MODIFY only if a consumed field is missing (then regenerate)

**Author (`alp-sdk-internal`, separate repo):**
- `meta-alp-sdk-internal/recipes-kernel/linux/linux-alif/e1m-aen801-hwconfig.dtsi` (or conf fragment) — resolved TF-A map + HyperRAM timings (public `require`s when present)

**Consume (Alif BSP, unmodified):** `devkit-e8.conf`, `linux-alif`, TF-A, SoC dtsi.

---

## Task 0: Build-env grounding (fetch + read the real Alif BSP)

Rationale: the exact base values (TF-A platform, defconfig, SoC dtsi include path, console) are NOT vendored locally (R2). Ground them from the real BSP before authoring overrides, so nothing is invented.

**Files:**
- Create: `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` (grounding capture; not shipped code)

**Interfaces:**
- Produces: the resolved base facts (SoC dtsi filename + include path, `KERNEL_DEVICETREE` form, TF-A platform string, console UART node, `xipImage` flow, devkit defconfig peripheral coverage) consumed by Tasks 2–6.

- [ ] **Step 1: Define the gate** — `meta-alif-ensemble@scarthgap` is fetched into the WSL Yocto tree and `bitbake-layers show-layers` lists it.

- [ ] **Step 2: Show it's unmet**

Run (WSL): `cd <yocto-tree> && bitbake-layers show-layers | grep -i alif`
Expected: no `meta-alif-ensemble` row (not yet added).

- [ ] **Step 3: Fetch + add the layer**

```bash
# WSL, in the Yocto build tree (sibling of meta-alp-sdk)
git clone -b scarthgap https://github.com/alifsemi/meta-alif-ensemble ../meta-alif-ensemble
bitbake-layers add-layer ../meta-alif-ensemble
```

- [ ] **Step 4: Read + capture the base** — read `conf/machine/devkit-e8.conf`, the `linux-alif` recipe + defconfig, and the SoC dtsi. Record in the grounding note: exact SoC dtsi `#include` path, the `KERNEL_DEVICETREE` pattern devkit uses, TF-A platform string, console UART, `xipImage` settings, and which E1M-EVK peripherals the devkit defconfig already enables (so Task 6 adds only the delta).

- [ ] **Step 5: Verify the gate**

Run (WSL): `bitbake-layers show-layers | grep -i alif`
Expected: `meta-alif-ensemble` listed.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md
git commit -q -m "docs(aen): capture devkit-e8 BSP grounding facts for A32 bring-up"
```

---

## Task 1: Metadata completeness + regenerate

Ensure the generators have every fact the DTS/machine consume; extend `metadata/**` (not generated files) if a field is missing.

**Files:**
- Modify (only if a gap found): `metadata/boards/e1m-evk.yaml`, `metadata/e1m_modules/E1M-AEN801.yaml`, `metadata/socs/alif/ensemble/e8.json`
- Verify-generated: `include/alp/boards/alp_e1m_evk_routes.h`, `include/alp/soc_caps.h`

**Interfaces:**
- Produces: confirmed metadata fields (console UART route, carrier I²C buses, OSPI nodes, MHUv2) referenced by Tasks 2–3.

- [ ] **Step 1: Define the gate** — `gen_board_header.py` + `gen_soc_caps.py` regenerate with no diff, and the generated-files gate passes.

- [ ] **Step 2: Show current state**

Run: `py -3.14 scripts/gen_board_header.py --check && py -3.14 scripts/gen_soc_caps.py --check` (or the repo's `--check` equivalent)
Expected: PASS (baseline) — establishes the regeneration is clean before edits.

- [ ] **Step 3: Audit the consumed fields** — confirm `e1m-evk.yaml` declares the console UART feature-macro + carrier I²C buses, and `E1M-AEN801.yaml` carries OSPI/HyperRAM/mailbox (it does — verify). If a DTS-needed fact is absent, add it to the YAML with a sourced value or `TBD`.

- [ ] **Step 4: Regenerate + verify the gate**

Run: `py -3.14 scripts/gen_board_header.py && py -3.14 scripts/gen_soc_caps.py` then the generated-files gate.
Expected: clean (no unexpected diff; any diff is an intended metadata addition).

- [ ] **Step 5: Commit** (skip if no metadata change needed)

```bash
git add metadata/ include/alp/boards/ include/alp/soc_caps.h
git commit -q -m "metadata(aen): complete E1M-EVK/AEN801 fields consumed by the A32 DTS"
```

---

## Task 2: E1M-AEN801 SoM device tree (`e1m-aen801-som.dtsi`)

Mirror `e1m-v2n-som.dtsi` (the working V2N SoM dtsi). On-module nodes only.

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-som.dtsi`
- Reference (read, do not edit): `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`

**Interfaces:**
- Produces: labels/nodes the carrier dtsi + product dts reference (`&ospi0`, HyperRAM region, `&mhu0`/mailbox, CC3501E SPI node).
- Consumes: SoC dtsi labels from the BSP (Task 0 grounding for the include + label names).

- [ ] **Step 1: Define the gate** — the dtsi is syntactically valid DT and, when included by the product dts (Task 4), `dtc` compiles without error.

- [ ] **Step 2: Author the SoM nodes** — sourced from `E1M-AEN801.yaml on_module` + `mailbox` + `pad_routes`:
  - OSPI0 controller enable; CS0 = `mx25um25645` (xSPI NOR, 32 MiB), CS1 = `w958d8nbya5i` HyperRAM (32 MiB). Partitions/role per preset.
  - `reserved-memory` for the on-die SRAM working set + any carveout SP3 needs (sized from `e8.json`).
  - MHUv2 mailbox node (`compatible` per the SoM preset's `alif,mhuv2-mbox` family; addresses/IRQs `TBD(alif-hw-config)` if not in the BSP SoC dtsi).
  - CC3501E SPI1 peripheral node (the `pad_routes` E1M_SPI1→cc3501e dispatch).
  - Regulators as needed.
  HW-sensitive leaf values (HyperRAM timing, exact addresses) → `TBD(alif-hw-config)` here; resolved copy goes to Task 9's internal overlay.

- [ ] **Step 3: Verify the gate** — deferred to Task 4 (needs the product dts + SoC dtsi to compile). For now, lint structure visually + `dtc -I dts -O dtb` on a minimal harness that includes only this dtsi with stub labels.

Run (WSL): `dtc -@ -I dts -O dtb -o /dev/null meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-som.dtsi 2>&1 | head` (expect only "no /chosen" style warnings, no hard errors on this fragment, or defer to Task 4 if it needs the SoC base).

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-som.dtsi
git commit -q -m "dts(aen): E1M-AEN801 SoM device tree (OSPI NOR+HyperRAM, CC3501E, MHUv2)"
```

---

## Task 3: E1M-EVK carrier device tree (`e1m-evk.dtsi`)

Mirror `e1m-x-evk.dtsi`. Carrier-level nodes from `metadata/boards/e1m-evk.yaml e1m_routes`.

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-evk.dtsi`
- Reference: `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`, `metadata/boards/e1m-evk.yaml`

**Interfaces:**
- Produces: carrier aliases (`serial0`, `i2c*`), `chosen { stdout-path; bootargs }`, GPIO-expander + I²C device nodes referenced by the product dts.
- Consumes: SoM labels from Task 2 (`&i2c*`, `&uart*`).

- [ ] **Step 1: Define the gate** — included by the product dts (Task 4), `dtc` compiles clean and `stdout-path` + `bootargs` resolve.

- [ ] **Step 2: Author the carrier nodes** — from `e1m-evk.yaml e1m_routes`: console UART pinmux+enable, carrier I²C buses + the populated devices (TCAL9538 GPIO expander, BMI323/ICM-42670/BMP581/INA236 are SP2's enable concern but their bus nodes belong here), aliases, `chosen` bootargs (XIP-appropriate: console on the carrier UART @115200, root from the NOR/initramfs). No on-module nodes (those are Task 2).

- [ ] **Step 3: Verify the gate** — deferred to Task 4.

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-evk.dtsi
git commit -q -m "dts(aen): E1M-EVK carrier device tree (console, I2C buses, GPIO expander)"
```

---

## Task 4: Product DTS + `linux-alif` bbappend

Compose SoC+SoM+carrier and wire the DTS into the kernel build. **This is the first hard `dtc` gate.**

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts`
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend`
- Reference: `e1m-v2n101-x-evk.dts`, `linux-renesas_%.bbappend`

**Interfaces:**
- Consumes: SoC dtsi (`#include` from BSP, Task 0), SoM dtsi (Task 2), carrier dtsi (Task 3).
- Produces: `e1m-aen801-evk.dtb` target consumed by Task 5's `KERNEL_DEVICETREE`.

- [ ] **Step 1: Define the gate** — `dtc`/kernel DT build of `e1m-aen801-evk.dts` produces a `.dtb` with no errors, and `bitbake -e linux-alif` shows the bbappend's `SRC_URI`/`FILESEXTRAPATHS` picking up the new files.

- [ ] **Step 2: Author the product dts**

```dts
// SPDX-License-Identifier: Apache-2.0
/dts-v1/;
#include "<alif-e8-soc>.dtsi"   /* from meta-alif-ensemble / linux-alif — exact name per Task 0 */
#include "e1m-aen801-som.dtsi"
#include "e1m-evk.dtsi"

/ {
    model = "ALP E1M-AEN801 on E1M-EVK (Alif Ensemble E8 / AE822)";
    compatible = "alp,e1m-aen801-evk", "alif,ae822";
    /* memory { } sourced from the OSPI HyperRAM + on-die SRAM map — TBD(alif-hw-config) addresses */
};
```

- [ ] **Step 3: Author the bbappend** (mirror `linux-renesas_%.bbappend`): `FILESEXTRAPATHS:prepend`, `SRC_URI += "file://e1m-aen801-som.dtsi file://e1m-evk.dtsi file://e1m-aen801-evk.dts"`, install the dts into the kernel `arch/arm64/boot/dts/...` tree under the path `KERNEL_DEVICETREE` expects.

- [ ] **Step 4: Verify the gate**

Run (WSL, in the Yocto env): `MACHINE=e1m-aen801-a32 bitbake -c compile linux-alif 2>&1 | tail -40` (or at minimum `bitbake -e linux-alif | grep -E "e1m-aen801-evk|FILESEXTRAPATHS"`).
Expected: the dtb builds (or the dts compiles via a standalone `dtc` with the BSP include path on `-i`); no DT errors.

- [ ] **Step 5: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend
git commit -q -m "dts(aen): product DTS + linux-alif bbappend for e1m-aen801-evk"
```

---

## Task 5: Machine conf fill-in

Uncomment/fill `e1m-aen801-a32.conf` from the Task 0 grounding; TBD what's carrier-HW-specific.

**Files:**
- Modify: `meta-alp-sdk/conf/machine/e1m-aen801-a32.conf`

**Interfaces:**
- Consumes: the `e1m-aen801-evk.dtb` from Task 4; Task 0's TF-A/boot facts.

- [ ] **Step 1: Define the gate** — `bitbake -e MACHINE=e1m-aen801-a32 alp-image-edge` resolves `KERNEL_DEVICETREE` to `e1m-aen801-evk.dtb` and `ALP_BOOT_DEVICE` to the OSPI NOR, with no parse error.

- [ ] **Step 2: Show it's unmet**

Run (WSL): `MACHINE=e1m-aen801-a32 bitbake -e alp-image-edge | grep -E "^KERNEL_DEVICETREE="`
Expected: empty / devkit default (the line is still commented out).

- [ ] **Step 3: Fill the conf** — uncomment + set:
  - `KERNEL_DEVICETREE = "alif/ensemble/e1m/e1m-aen801-evk.dtb"` (path per Task 0)
  - `ALP_BOOT_DEVICE = "ospi-nor"` (OSPI0 CS0)
  - keep `require conf/machine/devkit-e8.conf` + `xipImage`
  - TF-A platform string: set from Task 0 grounding if known, else leave `TBD(alif-hw-config)` with the resolved value going to Task 9's internal overlay (`require`d when present).

- [ ] **Step 4: Verify the gate**

Run (WSL): `MACHINE=e1m-aen801-a32 bitbake -e alp-image-edge | grep -E "^(KERNEL_DEVICETREE|ALP_BOOT_DEVICE)="`
Expected: both set as above.

- [ ] **Step 5: Commit**

```bash
git add meta-alp-sdk/conf/machine/e1m-aen801-a32.conf
git commit -q -m "machine(aen): wire KERNEL_DEVICETREE + boot device for e1m-aen801-a32"
```

---

## Task 6: Kernel defconfig fragment

Enable only the E1M-EVK peripherals the devkit defconfig lacks (audited in Task 0).

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.cfg`
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend` (add the `.cfg` to `SRC_URI`)

**Interfaces:**
- Consumes: Task 0's devkit-defconfig coverage audit (add only the delta).

- [ ] **Step 1: Define the gate** — `bitbake -c kernel_configme linux-alif` applies the fragment and the target symbols are set in `.config`.

- [ ] **Step 2: Author the fragment** — `CONFIG_*` for: OSPI NOR (`MTD_SPI_NOR`/the Alif OSPI driver if not already on), HyperRAM/OSPI RAM, carrier I²C/SPI/GPIO controllers, `MAILBOX` + the MHUv2/remoteproc + rpmsg classes (for SP3), squashfs/overlayfs (XIP rootfs). Only symbols NOT already enabled by devkit defconfig.

- [ ] **Step 3: Verify the gate**

Run (WSL): `MACHINE=e1m-aen801-a32 bitbake -c kernel_configme linux-alif` then grep the resulting `.config` for the target symbols.
Expected: each target `CONFIG_*` present.

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.cfg meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend
git commit -q -m "kernel(aen): defconfig fragment for E1M-EVK peripherals + IPC"
```

---

## Task 7: Image recipe XIP/size knobs

Machine-conditioned knobs in the shared `alp-image-edge.bb` (unification — no AEN-specific recipe).

**Files:**
- Modify: `meta-alp-sdk/recipes-images/alp-image-edge.bb`

**Interfaces:**
- Consumes: `xipImage` from `devkit-e8`; the XIP memory model.

- [ ] **Step 1: Define the gate** — `bitbake -e -e1m-aen801-a32 alp-image-edge` shows the image type = `xipImage`/squashfs-appropriate and a footprint-bounded `IMAGE_INSTALL` for the AEN machine, while V2N's resolution is unchanged.

- [ ] **Step 2: Add machine-conditioned knobs** — use `:e1m` / `:e1m-aen801-a32` overrides (the conf sets `MACHINEOVERRIDES`) to select an XIP-friendly `IMAGE_FSTYPES`, a RAM-bounded rootfs, and (per §8.2) decide ROS2 inclusion: keep `rclcpp sensor-msgs` if it fits the footprint, else gate it behind a machine override and log the trim. Do NOT alter the default (V2N) path.

- [ ] **Step 3: Verify the gate**

Run (WSL): `for M in e1m-v2n101-a55 e1m-aen801-a32; do echo "== $M =="; MACHINE=$M bitbake -e alp-image-edge | grep -E "^(IMAGE_FSTYPES|IMAGE_INSTALL)="; done`
Expected: V2N unchanged; AEN shows the XIP/size-bounded values.

- [ ] **Step 4: Commit**

```bash
git add meta-alp-sdk/recipes-images/alp-image-edge.bb
git commit -q -m "image(aen): machine-conditioned XIP/size knobs for e1m-aen801-a32"
```

---

## Task 8: CI bitbake matrix row

**Files:**
- Modify: `.github/workflows/pr-bitbake.yml`

- [ ] **Step 1: Define the gate** — the workflow YAML parses and lists `e1m-aen801-a32` in the machine matrix alongside the V2N/NX9101 rows.

- [ ] **Step 2: Show it's unmet**

Run: `grep -n "e1m-aen801-a32" .github/workflows/pr-bitbake.yml`
Expected: no match.

- [ ] **Step 3: Add the matrix row** — mirror the existing V2N row (same steps/layers; add `meta-alif-ensemble` clone if the workflow fetches vendor layers per machine). Keep the bake scope identical to what V2N runs (config/parse/compile to the runner's limit).

- [ ] **Step 4: Verify the gate**

Run: `python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/pr-bitbake.yml')); print('yaml ok')"` and `grep -n "e1m-aen801-a32" .github/workflows/pr-bitbake.yml`
Expected: `yaml ok` + the row present.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/pr-bitbake.yml
git commit -q -m "ci(aen): add e1m-aen801-a32 to the pr-bitbake machine matrix"
```

---

## Task 9: Public/internal HW-value split

Move resolved HW-sensitive values out of the public tree into `alp-sdk-internal`; public `require`s them when present.

**Files:**
- Create (in `alp-sdk-internal`): `meta-alp-sdk-internal/recipes-kernel/linux/linux-alif/e1m-aen801-hwconfig.dtsi` and/or a conf fragment with the resolved TF-A map + HyperRAM timings.
- Modify (public): the SoM dtsi / machine conf to `require`/include the internal overlay only when the layer is present (graceful absence).

**Interfaces:**
- Consumes: any resolved values from Task 0 that are HW-sensitive.

- [ ] **Step 1: Define the gate** — `classifying-public-vs-internal` checks pass: no TF-A load addresses, HyperRAM timings, or schematic-derived values in the public tree; the public build still parses with the internal overlay ABSENT (values stay `TBD`).

- [ ] **Step 2: Audit + move** — scan the public DTS/conf for any concrete HW-sensitive value that slipped in; move it to the internal overlay, leave a `TBD(alif-hw-config)` + optional `require`-if-present hook in public.

- [ ] **Step 3: Verify the gate**

Run: the skill's checks — `git grep -nE "0x[0-9a-fA-F]{6,}" meta-alp-sdk/recipes-kernel/linux/linux-alif/` (review hits: addresses must be TBD or BSP-sourced, not carrier-secret) + a public-only parse `MACHINE=e1m-aen801-a32 bitbake -e alp-image-edge` with the internal layer not on bblayers.
Expected: no carrier-secret values in public; public parse succeeds with TBDs.

- [ ] **Step 4: Commit** (public side; internal side committed in `alp-sdk-internal` per `syncing-internal-repo`)

```bash
git add meta-alp-sdk/
git commit -q -m "split(aen): keep resolved TF-A/HyperRAM detail in alp-sdk-internal"
```

---

## Task 10: Docs

**Files:**
- Modify: `docs/bring-up-aen.md` (add the A32 Linux build section), `docs/os-support-matrix.md` (update AEN A32 status if it changes), `CHANGELOG.md` (Unreleased entry)

- [ ] **Step 1: Define the gate** — `scripts/check_doc_drift.py` passes and the bring-up doc documents `MACHINE=e1m-aen801-a32 bitbake alp-image-edge` + the XIP memory model + the board-needed-for-boot caveat.

- [ ] **Step 2: Write the docs** — A32 Linux build steps (clone `meta-alif-ensemble`, set MACHINE, bake), the OSPI/XIP memory note, and an explicit "on-target boot/peripheral validation pending hardware" status. Add the CHANGELOG `### Added` entry under `[Unreleased]`.

- [ ] **Step 3: Verify the gate**

Run: `py -3.14 scripts/check_doc_drift.py`
Expected: `doc-drift: OK`.

- [ ] **Step 4: Commit**

```bash
git add docs/bring-up-aen.md docs/os-support-matrix.md CHANGELOG.md
git commit -q -m "docs(aen): document the A32 Yocto/Linux build + XIP memory model"
```

---

## Task 11: Full no-board validation pass

Run the complete §10 gate set and attempt a full bake + size report (R1 feasibility).

**Files:** none (validation only); fix-forward any failure in the owning task's file.

- [ ] **Step 1: Run all gates**

```bash
# DTS compiles (WSL, with BSP include path)
MACHINE=e1m-aen801-a32 bitbake -c compile linux-alif
# image parse/configure
MACHINE=e1m-aen801-a32 bitbake -c configure alp-image-edge
# generated files clean
py -3.14 scripts/gen_board_header.py --check && py -3.14 scripts/gen_soc_caps.py --check
# vendor-clean portable surface
git grep -nE '#if(def)? .*(ALIF)_' -- include/alp/ src/common/ ':!include/alp/soc_caps.h'
git grep -niE '\balif[_a-z0-9]*\(' -- include/alp/ src/common/
# no-invented-values: every non-TBD HW value traces to a source (manual review of the diff)
# docs
py -3.14 scripts/check_doc_drift.py
```
Expected: DTS compiles; image configures; generated clean; vendor-clean greps empty; doc-drift OK.

- [ ] **Step 2: Attempt a full bake + capture footprint (R1)**

Run (WSL): `MACHINE=e1m-aen801-a32 bitbake alp-image-edge 2>&1 | tee /tmp/aen-bake.log`
Record: success/fail, rootfs + kernel size vs the ~32 MiB NOR / ~42 MiB RAM budget. If it overflows, apply the §8.2 trim (Task 7) and note it; an overflow here is the headline finding, not a failure of the plan.

- [ ] **Step 3: Write the validation summary** into `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md` (append): what built, what's `TBD`, the footprint result, and the ordered bench checklist (boot, console, OSPI, peripherals, MHUv2) for when the board arrives.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md
git commit -q -m "validate(aen): no-board gate pass + footprint report for SP1"
```

---

## Self-Review

**Spec coverage:** §2 principles → enforced in Global Constraints + per task. §3 memory model → Tasks 2/6/7. §4.1 DTS → Tasks 2-4. §4.2 bbappend → Task 4. §4.3 machine conf → Task 5. §4.4 kernel cfg → Task 6. §4.5 image → Task 7. §4.6 CI → Task 8. §4.7 metadata → Task 1. §5 public/internal → Task 9. §10 validation → Task 11. §7 TBDs → carried throughout. All covered.

**Placeholder scan:** The `TBD(alif-hw-config)` markers are spec-mandated doctrine (no-invented-values), not lazy plan placeholders — each names its source (Task 0 grounding) and its resolution path (internal overlay). DTS leaf values are sourced, not invented. No "implement later"/"add error handling"-style gaps.

**Type/name consistency:** File names consistent across tasks (`e1m-aen801-som.dtsi`, `e1m-evk.dtsi`, `e1m-aen801-evk.dts`, `linux-alif_%.bbappend`, `e1m-aen801-evk.cfg`, machine `e1m-aen801-a32`). `KERNEL_DEVICETREE` path consistent (Task 4 produces, Task 5 consumes). Gate commands consistent (`bitbake -e`, `dtc`, `gen_*`, `check_doc_drift`).

**Note on TDD adaptation:** SP1 has no unit-testable units; each task's "gate" is a build/lint check (dtc compile, bitbake parse/configure/kernel_configme, generated-files, vendor-clean grep, doc-drift) defined → shown unmet → made to pass → committed. That is the SP1 analogue of red-green-commit. SP2 (peripheral backends) will carry real `alp_*` tests.
