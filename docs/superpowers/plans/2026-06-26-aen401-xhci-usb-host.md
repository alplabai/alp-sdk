# E1M-AEN401 xHCI USB Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the wrong-IP DWC2 USB-host work with an **xHCI** USB-2.0 host `uhc` driver for the Alif Ensemble USB (DWC3-family), grounded from the E4 HWRM/DFP, building no-board for `alp_e1m_aen401_m55_hp`.

**Architecture:** Ground the real peripherals/memory/USB address from the Alif DFP `soc_features.h` (T1). Replace `uhc_dwc2_alif.*` with a standard **xHCI host core (per the xHCI spec) + thin Alif DWC3 `G*` glue** (T2), grounded register base `0x48200000` / IRQ 101. Re-point the `usbh` backend + board + example to the xHCI compatible (T3). Dispose of the DWC2 artifacts + re-point PR #268 (T4). The xHCI register map + DWC3 init + `uhc_api` scaffold compile; the ring/transfer/enumeration bring-up is bench-gated.

**Tech Stack:** Zephyr 4.4 (`uhc`/`usbh`), xHCI USB-2.0 (Synopsys DWC3-family), C (Zephyr driver style), `py -3.14` (metadata), west, WSL.

## Global Constraints

- Never invent register offsets — transcribe the controller base/IRQ + DWC3 `G*` init from the E4 HWRM §14.10 / DFP `soc.h` (**facts only — never the confidential doc, its OneDrive path, or verbatim HWRM prose**); the xHCI register *semantics* come from the public xHCI specification.
- New C under `zephyr/**` follows Zephyr driver style (the clang-format-22 gate EXCLUDES `zephyr/**`); C under `src/**` obeys clang-format-22 (`~/.local/bin/clang-format` 22.1.5 in WSL).
- The xHCI driver is a grounded, COMPILE-ONLY skeleton: ring/transfer/enumeration paths needing the live controller return a defined status + carry `TODO(aen401-bench)`. Do NOT claim runtime correctness.
- "Alp Lab" spelling; no `Co-Authored-By: Claude`; `py -3.14`; no binaries in git; no OneDrive paths / confidential prose in the repo.
- Branch `feat/aen401-usb-host` (off `dev`) → PR #268.
- Grounded values (DFP): USB `reg = <0x48200000 …>`, `interrupts = <101 0>`; DWC3 `GCTL @ 0xC110`. E4: SRAM0 `0x02000000`/4 MB, SRAM1 `0x08000000`/4 MB, MRAM `0x80000000`/5.5 MB, HP-ITCM `0x50000000`/256 KB, HP-DTCM `0x50800000`/1 MB, USB 16 EP. E7: SRAM1 2.5 MB, USB 8 EP. E8: SRAM1 4 MB, USB 16 EP.
- WSL Zephyr workspace `/home/alplab/zephyrproject` (4.4.0); drive west via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`.

---

### Task 1: C0 — ground peripherals/memory/USB from the DFP

**Files:**
- Modify: `metadata/socs/alif/ensemble/e4.json`, `e7.json`, `e8.json` (`peripherals`, `memory_regions` if present)
- Modify: `zephyr/boards/alp/e1m_aen401_m55_hp/alp_e1m_aen401_m55_hp_ae402fa0e5597le0_rtss_hp.dts` (memory map + USB node)

**Interfaces:**
- Produces: e4/e7/e8 `peripherals.usb_2 = 1` (real, replacing the E3-sibling guess) + the grounded memory map; the AEN401 board USB node at `0x48200000`/IRQ 101.

- [ ] **Step 1: Replace the e4.json sibling-guess with the DFP-grounded peripheral + note**

In `e4.json`, keep `peripherals.usb_2: 1` but rewrite its `notes[]` entry to: `"USB = xHCI USB-2.0 dual-role @ 0x48200000 (IRQ 101), 16 endpoints; grounded from the Alif DFP soc.h/soc_features.h (AE402FA0E5597). Host mode supported (E4 datasheet 'USB 2.0 HS/FS Host/Device')."` Add `usb_2: 1` to `e7.json` and `e8.json` `peripherals` with an analogous DFP-grounded note (E7 = 8 EP, E8 = 16 EP).

- [ ] **Step 2: Validate the metadata**

```
py -3.14 -c "import json,jsonschema; s=json.load(open('metadata/schemas/soc-spec-v1.schema.json')); [jsonschema.validate(json.load(open(f)),s) for f in ['metadata/socs/alif/ensemble/e4.json','metadata/socs/alif/ensemble/e7.json','metadata/socs/alif/ensemble/e8.json']]; print('e4/e7/e8 validate')"
```
Expected: `e4/e7/e8 validate`.

- [ ] **Step 3: Correct the AEN401 board USB node + memory map**

In the board DTS, replace the heuristic USB node (`usb@4014c000`, `alif,dwc2-uhc`, `reg=<0x4014c000 0x10000>`, `interrupts=<164 0>`) with the grounded xHCI node:
```dts
		zephyr_uhc0: usb@48200000 {
			compatible = "alif,xhci-uhc";
			reg = <0x48200000 0x10000>;
			interrupts = <101 0>;
			maximum-speed = "high-speed";
			status = "okay";
		};
```
Confirm the MRAM/SRAM/TCM nodes match the DFP E4 values (MRAM `0x80000000`/5.5 MB; if the SRAM/TCM nodes carry `TODO(aen401-bench)` heuristics, set them to the grounded E4 values: SRAM0 `0x02000000`/4 MB, SRAM1 `0x08000000`/4 MB, HP-ITCM `0x50000000`/256 KB, HP-DTCM `0x50800000`/1 MB). Drop the now-resolved `TODO(aen401-bench)` on these.

- [ ] **Step 4: Commit**

```bash
git add metadata/socs/alif/ensemble/e4.json metadata/socs/alif/ensemble/e7.json metadata/socs/alif/ensemble/e8.json zephyr/boards/alp/e1m_aen401_m55_hp/
git commit -m "metadata+dts(aen401): ground USB (xHCI @0x48200000/IRQ101) + E4/E7/E8 peripherals from the Alif DFP"
```

---

### Task 2: C1/C4 — xHCI host `uhc` driver (replaces the DWC2 driver)

**Files:**
- Delete: `zephyr/drivers/usb/uhc/uhc_dwc2_alif.c`, `zephyr/drivers/usb/uhc/Kconfig.dwc2_alif`, `zephyr/dts/bindings/usb/alif,dwc2-uhc.yaml`
- Create: `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`, `zephyr/drivers/usb/uhc/Kconfig.xhci_alif`, `zephyr/dts/bindings/usb/alif,xhci-uhc.yaml`
- Modify: `zephyr/drivers/usb/uhc/Kconfig`, `zephyr/CMakeLists.txt`, `zephyr/Kconfig` (swap the dwc2→xhci references)
- Reference (WSL): `~/zephyrproject/zephyr/include/zephyr/drivers/usb/uhc.h` (the `uhc_api`), `uhc_max3421e.c` (op-table shape); the **xHCI specification** for register/ring/context semantics; the E4 HWRM §14.10.5.3/§14.10.6 for the base + DWC3 `G*` init (facts only)

**Interfaces:**
- Produces: a `uhc` device bound to `compatible = "alif,xhci-uhc"` exposing a complete `struct uhc_api`; `CONFIG_UHC_XHCI_ALIF`.

- [ ] **Step 1: Remove the DWC2 driver + its wiring**

```bash
git rm zephyr/drivers/usb/uhc/uhc_dwc2_alif.c zephyr/drivers/usb/uhc/Kconfig.dwc2_alif zephyr/dts/bindings/usb/alif,dwc2-uhc.yaml
```
In `zephyr/CMakeLists.txt` + `zephyr/Kconfig` + `zephyr/drivers/usb/uhc/Kconfig`, remove the `uhc_dwc2_alif` / `Kconfig.dwc2_alif` references (you'll add the xhci ones in Step 5).

- [ ] **Step 2: DT binding** `zephyr/dts/bindings/usb/alif,xhci-uhc.yaml`

```yaml
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
description: Alif Ensemble USB controller (DWC3-family) in xHCI host mode

compatible: "alif,xhci-uhc"

include: [base.yaml]

properties:
  reg:
    required: true
  interrupts:
    required: true
  maximum-speed:
    type: string
    enum: ["high-speed", "full-speed"]
    default: "high-speed"
```

- [ ] **Step 3: Ground the controller base + DWC3 `G*` init (WSL, facts only)**

Confirm the xHCI base + IRQ + the DWC3 global init from the DFP/HWRM (do NOT copy HWRM prose into code — extract the offset/step facts):
```
MSYS_NO_PATHCONV=1 wsl bash -lc "grep -nE 'USB_BASE|USB_IRQ_IRQn|GCTL' /home/alplab/alif-bsp-ref/alif-usb-ref/alif_ensemble-cmsis-dfp/Device/soc/AE402FA0E5597/include/rtss_hp/soc.h | head"
```
Expected: `USB_BASE 0x48200000`, `USB_IRQ_IRQn = 101`, `GCTL @ 0xC110`. Record the DWC3 `G*` host-mode steps from HWRM §14.10.5.3 / Table 14-168 as code comments (which `G*` registers to program; `GCTL` PrtCapDir=host).

- [ ] **Step 4: Write the xHCI host driver** `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`

A standard xHCI host core + Alif DWC3 glue. The xHCI register/struct definitions are spec-standard (write them from the xHCI spec); the controller init programs the DWC3 `G*` registers then the xHCI CAP/operational registers. Transfer/enumeration paths that need the live controller return a defined status + carry `TODO(aen401-bench)`. Scaffold (fill the xHCI struct/register bodies from the spec; DWC3 init from Step 3):
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble USB host (xHCI USB-2.0 dual-role, DWC3-family) UHC driver.
 *
 * SKELETON: the xHCI register map (CAP/op/runtime/doorbell @ 0x48200000),
 * the standard xHCI ring/context structures (per the xHCI spec), the DWC3
 * G*-register host-mode init (HWRM §14.10.5.3), and the uhc_api wiring are
 * real; the ring processing / transfer completion / event ISR / root-hub
 * enumeration that need the live controller are TODO(aen401-bench).  Not a
 * validated driver.  GPL Linux/u-boot xHCI drivers are reference-only.
 */
#define DT_DRV_COMPAT alif_xhci_uhc

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>
#include "uhc_common.h"

LOG_MODULE_REGISTER(uhc_xhci_alif, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* DWC3 global registers (offsets from the controller base; GCTL @ 0xC110). */
#define DWC3_GCTL              0xC110u
#define DWC3_GCTL_PRTCAPDIR_SHIFT 12
#define DWC3_GCTL_PRTCAPDIR_HOST  (1u << DWC3_GCTL_PRTCAPDIR_SHIFT)
/* TODO(aen401-bench): GUSB2PHYCFG0 + GTXFIFOSIZ/GRXFIFOSIZ offsets + values
 * from HWRM §14.10.5.3 Table 14-168. */

/* xHCI capability registers (base = controller base; per xHCI spec §5.3). */
struct xhci_cap_regs {
	uint8_t  caplength;
	uint8_t  rsvd;
	uint16_t hciversion;
	uint32_t hcsparams1;
	uint32_t hcsparams2;
	uint32_t hcsparams3;
	uint32_t hccparams1;
	uint32_t dboff;   /* doorbell array offset */
	uint32_t rtsoff;  /* runtime register space offset */
	uint32_t hccparams2;
};
/* TODO(aen401-bench): operational (CAPLENGTH offset), runtime (RTSOFF),
 * doorbell (DBOFF) register maps + the command/event/transfer TRB rings +
 * DCBAA + device contexts, per the xHCI spec §5-6. */

struct uhc_xhci_alif_config {
	uintptr_t base;          /* 0x48200000 from DT */
	void (*irq_config)(void);
};
struct uhc_xhci_alif_data {
	struct uhc_data common;  /* MUST be first */
	volatile struct xhci_cap_regs *cap;
};

static int uhc_xhci_alif_lock(const struct device *dev) { return uhc_lock_internal(dev, K_FOREVER); }
static int uhc_xhci_alif_unlock(const struct device *dev) { return uhc_unlock_internal(dev); }

static int uhc_xhci_alif_init(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg = dev->config;
	struct uhc_xhci_alif_data *data = dev->data;

	data->cap = (volatile struct xhci_cap_regs *)cfg->base;
	/* TODO(aen401-bench): DWC3 G* host-mode init (GCTL PrtCapDir=host,
	 * GUSB2PHYCFG0, FIFO sizes) per HWRM §14.10.5.3; then xHCI: read
	 * CAPLENGTH/HCSPARAMS, locate op/runtime/doorbell, alloc DCBAA +
	 * command ring + event ring (ERST), set CONFIG.MaxSlotsEn. */
	return 0;
}
static int uhc_xhci_alif_enable(const struct device *dev)
{
	/* TODO(aen401-bench): set USBCMD.R/S, unmask the primary interrupter,
	 * enable the global IRQ; power the root-hub port (PORTSC.PP). */
	return 0;
}
static int uhc_xhci_alif_disable(const struct device *dev) { return 0; /* TODO(aen401-bench) */ }
static int uhc_xhci_alif_shutdown(const struct device *dev) { return 0; /* TODO(aen401-bench) */ }
static int uhc_xhci_alif_bus_reset(const struct device *dev)
{
	/* TODO(aen401-bench): PORTSC.PR (port reset), wait PRC. */
	return 0;
}
static int uhc_xhci_alif_sof_enable(const struct device *dev) { return 0; /* TODO(aen401-bench) */ }
static int uhc_xhci_alif_bus_suspend(const struct device *dev) { return 0; /* TODO(aen401-bench) */ }
static int uhc_xhci_alif_bus_resume(const struct device *dev) { return 0; /* TODO(aen401-bench) */ }
static int uhc_xhci_alif_ep_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	/* TODO(aen401-bench): map the transfer to a slot/endpoint transfer
	 * ring, enqueue Normal/Setup/Data/Status TRBs, ring the doorbell;
	 * completion via the event-ring ISR. */
	return -ENOTSUP;
}
static int uhc_xhci_alif_ep_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	return -ENOTSUP; /* TODO(aen401-bench) */
}

static const struct uhc_api uhc_xhci_alif_api = {
	.lock = uhc_xhci_alif_lock,
	.unlock = uhc_xhci_alif_unlock,
	.init = uhc_xhci_alif_init,
	.enable = uhc_xhci_alif_enable,
	.disable = uhc_xhci_alif_disable,
	.shutdown = uhc_xhci_alif_shutdown,
	.bus_reset = uhc_xhci_alif_bus_reset,
	.sof_enable = uhc_xhci_alif_sof_enable,
	.bus_suspend = uhc_xhci_alif_bus_suspend,
	.bus_resume = uhc_xhci_alif_bus_resume,
	.ep_enqueue = uhc_xhci_alif_ep_enqueue,
	.ep_dequeue = uhc_xhci_alif_ep_dequeue,
};

static int uhc_xhci_alif_driver_init(const struct device *dev)
{
	struct uhc_xhci_alif_data *data = dev->data;

	k_mutex_init(&data->common.mutex);
	/* TODO(aen401-bench): map base, connect the IRQ via cfg->irq_config. */
	return 0;
}

#define UHC_XHCI_ALIF_INIT(n)                                                        \
	static void uhc_xhci_alif_irq_config_##n(void) {                             \
		/* TODO(aen401-bench): IRQ_CONNECT(DT_INST_IRQN(n), ...) */           \
	}                                                                            \
	static const struct uhc_xhci_alif_config uhc_xhci_alif_cfg_##n = {           \
		.base = DT_INST_REG_ADDR(n),                                         \
		.irq_config = uhc_xhci_alif_irq_config_##n,                          \
	};                                                                           \
	static struct uhc_xhci_alif_data uhc_xhci_alif_data_##n;                     \
	DEVICE_DT_INST_DEFINE(n, uhc_xhci_alif_driver_init, NULL,                    \
			      &uhc_xhci_alif_data_##n, &uhc_xhci_alif_cfg_##n,        \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,        \
			      &uhc_xhci_alif_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_XHCI_ALIF_INIT)
```
(Verify `struct uhc_data` / `uhc_lock_internal` against the WSL `uhc_common.h` and adjust if the 4.4 API differs.)

- [ ] **Step 5: Kconfig + CMake**

`zephyr/drivers/usb/uhc/Kconfig.xhci_alif`:
```kconfig
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
config UHC_XHCI_ALIF
	bool "Alif Ensemble xHCI USB host controller driver"
	default y
	depends on DT_HAS_ALIF_XHCI_UHC_ENABLED
	help
	  xHCI USB-2.0 host driver for the Alif Ensemble USB (DWC3-family).
	  Host bring-up paths are bench-gated (TODO(aen401-bench)).
```
`zephyr/drivers/usb/uhc/Kconfig`: `rsource "Kconfig.xhci_alif"`. In `zephyr/CMakeLists.txt`, swap the dwc2 source line to `uhc_xhci_alif.c` (mirror the existing flat-path wiring).

- [ ] **Step 6: Compile-check (WSL)**

Build the example (from Task 3, once it exists) or a minimal uhc app for the AEN401 board with `CONFIG_UHC_DRIVER=y CONFIG_UHC_XHCI_ALIF=y`. The driver must compile + link (the `uhc_api` symbols resolve).
Expected: link-clean; warnings noted.

- [ ] **Step 7: Commit**

```bash
git add zephyr/drivers/usb/ zephyr/dts/bindings/usb/ zephyr/CMakeLists.txt zephyr/Kconfig
git commit -m "drivers(usb): xHCI host UHC driver for Alif Ensemble (replaces DWC2; bring-up bench-gated)"
```

---

### Task 3: C2/C3 — re-point the backend + board chosen-node; build for AEN401

**Files:**
- Modify: `src/backends/usb/zephyr_drv.c` (the host-stack guard + the `USBH_CONTROLLER_DEFINE` compat)
- Modify: `zephyr/boards/alp/e1m_aen401_m55_hp/...rtss_hp.dts` (`chosen { zephyr,uhc = &zephyr_uhc0; }` already points at the node Task 1 renamed)
- Modify: `examples/peripheral-io/usb-host-storage/prj.conf` (swap `CONFIG_UHC_DWC2_ALIF` → `CONFIG_UHC_XHCI_ALIF`)

**Interfaces:**
- Consumes: Task 2's `alif,xhci-uhc` driver + Task 1's board USB node.

- [ ] **Step 1: Re-point the backend compat**

In `src/backends/usb/zephyr_drv.c`, change the host guard `DT_HAS_COMPAT_STATUS_OKAY(alif_dwc2_uhc)` → `DT_HAS_COMPAT_STATUS_OKAY(alif_xhci_uhc)` (the `USBH_CONTROLLER_DEFINE(alp_usbh, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)))` node-label is unchanged). Keep the `#else` NOSUPPORT path.

- [ ] **Step 2: clang-format-22 the backend file** (it's under `src/**`)

```
MSYS_NO_PATHCONV=1 wsl bash -lc "cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && ~/.local/bin/clang-format -i src/backends/usb/zephyr_drv.c && ~/.local/bin/clang-format --dry-run --Werror src/backends/usb/zephyr_drv.c && echo CLANGFMT-CLEAN"
```
Expected: `CLANGFMT-CLEAN`.

- [ ] **Step 3: Swap the example Kconfig**

In `examples/peripheral-io/usb-host-storage/prj.conf`, replace `CONFIG_UHC_DWC2_ALIF=y` with `CONFIG_UHC_XHCI_ALIF=y`. (Keep `CONFIG_USB_HOST_STACK=y`, `CONFIG_UHC_DRIVER=y`.)

- [ ] **Step 4: Build for AEN401 (WSL — the no-board gate)**

`scratchpad/build_xhci.sh`:
```bash
cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk
export ALP_SDK_ROOT=$PWD
west build -b alp_e1m_aen401_m55_hp examples/peripheral-io/usb-host-storage -d /tmp/xhci_host -p always 2>&1 | tail -30
file /tmp/xhci_host/zephyr/zephyr.elf
```
Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/build_xhci.sh`.
Expected: the xHCI driver + the `z_host_*` wiring + the example link into a Cortex-M55 ELF. **This is the no-board validation.** If a Zephyr `usbh`/`uhc` API mismatch blocks it, fix it (or report BLOCKED with the exact error).

- [ ] **Step 5: Commit**

```bash
git add src/backends/usb/zephyr_drv.c examples/peripheral-io/usb-host-storage/prj.conf
git commit -m "backend+example(aen401): re-point USB host to the xHCI driver; build for alp_e1m_aen401_m55_hp"
```

---

### Task 4: C4 — disposition (PR + docs)

**Files:**
- Modify: `docs/superpowers/plans/2026-06-26-aen401-usb-host.md` (supersede banner)
- Modify: `examples/peripheral-io/usb-host-storage/README.md` (xHCI, not DWC2)
- Modify: `CHANGELOG.md`
- Create: `docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md` (the build evidence + the bench TODO list)

- [ ] **Step 1: Supersede the DWC2 plan + fix the README**

Add a `> **SUPERSEDED** by 2026-06-26-aen401-xhci-usb-host.md (the IP is xHCI, not DWC2).` banner to the top of `docs/superpowers/plans/2026-06-26-aen401-usb-host.md`. In the example README, replace DWC2/`uhc_dwc2_alif` references with xHCI/`uhc_xhci_alif`, the real USB address (`0x48200000`/IRQ 101), and the honest "compile-only skeleton; enumeration bench-gated" framing.

- [ ] **Step 2: CHANGELOG + grounding note**

Add a CHANGELOG entry (AEN401 USB host re-based on the correct xHCI IP; DWC2 driver removed; USB grounded `0x48200000`/IRQ 101 from the DFP). Write the grounding note with the build-ELF arch + the `TODO(aen401-bench)` list. Run `py -3.14 scripts/check_doc_drift.py` (expect OK).

- [ ] **Step 3: Re-point PR #268**

```bash
git push
gh pr edit 268 --title "AEN401 M55/Zephyr USB host (xHCI, on-SoC DWC3-family)" --body-file <updated body noting the xHCI correction>
gh pr ready 268   # un-draft once the build is green
```

- [ ] **Step 4: Commit**

```bash
git add docs/ examples/peripheral-io/usb-host-storage/README.md CHANGELOG.md
git commit -m "docs(aen401): xHCI USB-host disposition — supersede DWC2, grounding note, CHANGELOG"
```

---

## Self-Review

**Spec coverage:**
- C0 (DFP grounding: e4/e7/e8 peripherals + memory + USB address; board USB node) → Task 1. ✅
- C1 (xHCI host uhc driver) + C4-driver-removal → Task 2. ✅
- C2 (backend re-point) + C3 (board chosen-node + example + build) → Task 3. ✅
- C4 (disposition: PR, supersede, docs) → Task 4. ✅
- No-board validation (compile for alp_e1m_aen401_m55_hp) → Task 3 Step 4. ✅
- Confidential-source rule (facts-only) → Global Constraints + Task 2 Step 3. ✅

**Placeholder scan:** The `TODO(aen401-bench)` markers are the spec's bench-gated boundaries (ring/transfer/enumeration, the DWC3 FIFO/PHY init values) — the register base, `GCTL` host-mode, the xHCI cap-struct, Kconfig/CMake/binding, backend re-point, board node, and example are concrete. The grounding steps (Task 2 Step 3) name the exact DFP file + the HWRM section for the facts. No vague "add error handling".

**Type/name consistency:** `compatible = "alif,xhci-uhc"` ↔ `DT_DRV_COMPAT alif_xhci_uhc` ↔ `DT_HAS_ALIF_XHCI_UHC_ENABLED` ↔ the binding filename ↔ `DT_HAS_COMPAT_STATUS_OKAY(alif_xhci_uhc)` (Task 3). `CONFIG_UHC_XHCI_ALIF` consistent across Kconfig/CMake/prj.conf. USB `0x48200000`/IRQ 101 consistent across metadata (T1), board DTS (T1), and the driver base (T2). `zephyr_uhc0` node label unchanged so the backend `DT_NODELABEL` still resolves. Board `alp_e1m_aen401_m55_hp` consistent.
