# E1M-AEN801 A32-cluster Yocto/Linux bring-up — design (SP1)

**Status:** design / pre-implementation
**Date:** 2026-06-25
**Branch:** `feat/aen-a32-yocto-bringup`
**Scope:** SP1 of a 3-sub-project effort (SP1 image bring-up → SP2 A32 `alp_*`
peripheral backends → SP3 A32/M55 multicore). This doc covers **SP1 only**;
SP2/SP3 get their own spec → plan when SP1 lands.

---

## 1. Goal

Stand up the Yocto/Linux software for the Alif Ensemble E8 **Cortex-A32 cluster**
on the **E1M-EVK** carrier (SoM = `E1M-AEN801`) **as far as is possible without
the physical board** — i.e. everything up to, but not including, on-target boot
and runtime validation.

Concretely, after SP1:
- `MACHINE=e1m-aen801-a32 bitbake alp-image-edge` parses, configures, and builds
  as far as fetch + `do_configure` (ideally a full image bake, footprint
  permitting).
- The carrier device tree **compiles** (`dtc`) and composes SoC + SoM + carrier.
- All generated artifacts (`soc_caps.h`, route headers) regenerate clean.
- Every value we cannot ground is a **clean `TBD(alif-hw-config)`**, never invented.

### Non-goals (SP1)
- On-board boot, console, peripheral runtime, IPC handshake — **deferred to bench**
  (the only thing gated on the physical board, per the maintainer).
- SP2 (`alp_*` peripheral backends on A32) and SP3 (A32/M55 multicore) — separate
  specs; SP1 only ensures the image and board layer exist for them to build on.

---

## 2. Design principles (non-negotiable)

These come from `securing-the-alp-sdk-position` (ADR 0017) and the unification
doctrine, and they shape every decision below.

### 2a. alp-sdk is a UNIFICATION layer — AEN A32 is "just another MACHINE"
The customer-facing experience for AEN-A32 Linux must be **identical** to V2N-A55
Linux. Same `board.yaml` shape, same `som.sku` + `preset` selection, same
`alp-image-edge` image recipe, same `alp_*` API, same generated-from-`metadata/**`
flow, same orchestrator path. Targeting AEN vs V2N is a **one-line `som.sku`
change**, nothing more. We do **not** build a parallel AEN Linux stack; we add one
more machine to the existing unified Yocto layer (`meta-alp-sdk`) and reuse every
piece of V2N machinery that is SoM-agnostic.

> Acceptance lens for every SP1 artifact: *"Does this make the SDK more unifying
> (one model, many silicon), or does it special-case AEN?"* If it special-cases,
> reshape it.

### 2b. Ride OVER the vendor SDK (ADR 0017 Tier-1) — author only the value-add
Consume `meta-alif-ensemble` (branch `scarthgap`) `devkit-e8` **as-is**: the
`cortexa32` tune, `linux-alif` kernel, TF-A platform, `xipImage`, SoC dtsi, serial
console, SMP. We **do not fork** it. Our value-add as the SoM manufacturer is the
thin **board layer**: the E1M SoM dtsi + E1M-EVK carrier dtsi + product dts, the
`linux-alif` bbappend, and the machine-conf carrier overrides. Nothing we author
re-implements something the Alif BSP already ships.

### 2c. Vendor-clean portable surface
No `alif_*` / `ALIF_*` tokens in `include/alp/**` or `src/common/**`. A32
peripheral access is `alp_*_open` + `ALP_E1M_*` IDs (that's SP2's concern; SP1 just
must not leak vendor names into anything portable). Vendor specifics live in
`meta-alp-sdk/`, `src/backends/`, `src/yocto/`, `zephyr/dts/`.

### 2d. One source of truth
Every HW fact lives once under `metadata/**` and downstream files are generated:
- Carrier pad routes → `metadata/boards/e1m-evk.yaml` → `gen_board_header.py` →
  `include/alp/boards/alp_e1m_evk_routes.h`.
- SoC caps → `metadata/socs/alif/ensemble/e8.json` → `gen_soc_caps.py`.
- SoM memory/OSPI/mailbox → `metadata/e1m_modules/E1M-AEN801.yaml`.
The device tree is the one **hand-authored** artifact (as for V2N) — it must
**reference** these facts, not re-encode contradicting copies.

---

## 3. The distinguishing constraint: an OSPI / XIP memory model

Unlike V2N (external LPDDR), the E1M-AEN801 SoM has **no parallel DRAM**. Memory is
on-die + OSPI-attached (per `E1M-AEN801.yaml on_module`):

| Region | Device | Size | Role |
|--------|--------|------|------|
| OSPI0 CS0 | MX25UM25645 OctaFlash (xSPI NOR) | 32 MiB | **ROM** — boot chain + app/kernel storage |
| OSPI0 CS1 | W958D8NBYA5I OctalRAM (HyperRAM) | 32 MiB | **RAM** — volatile XIP/working RAM |
| on-die SRAM | — | ~9984 KiB | RAM (kernel/working set) |
| on-die MRAM | — | 5.5 MiB | NV (boot/secure) |
| OSPI1 | TBD (BOM variant) | TBD | unused on AEN801 today |

Both OSPI0 devices share one octal controller, split by chip-select (NOR=CS0,
HyperRAM=CS1). The two OSPI memories are **BOM-optional and either can be populated
as RAM or ROM** — so the RAM/ROM assignment is a **per-SoM/board parameter**, not a
fixed map. Default (AEN801 r1): NOR=ROM on CS0, HyperRAM=RAM on CS1.

**Consequence — this is an XIP-class Linux, not a DRAM Linux:**
- Kernel runs **XIP from OSPI0 NOR** (`xipImage`, already inherited from
  `devkit-e8.conf`) to spare RAM.
- Working RAM ≈ 32 MiB HyperRAM + on-die SRAM (~42 MiB ceiling).
- Rootfs is small + read-mostly: squashfs/initramfs in NOR, RW overlay in RAM.
- This reshapes the **image recipe** and **kernel config** relative to V2N (see §4.5).

This memory model is the single biggest way SP1 differs from the V2N template, and
it is the main risk (§9).

---

## 4. Components to author (the value-add board layer)

Pattern-matched to the V2N set (which is the working reference). Tiers per ADR 0017.

### 4.1 Device-tree set — **author** (Tier-1: only board overlay + DT nodes)
Paralleling V2N's `e1m-v2n-som.dtsi` / `e1m-x-evk.dtsi` /
`e1m-v2n101-x-evk.dts`, under `meta-alp-sdk/recipes-kernel/linux/linux-alif/`:

| File | Contents | Source of truth |
|------|----------|-----------------|
| `e1m-aen801-som.dtsi` | on-module: OSPI0 NOR+HyperRAM nodes, CC3501E, on-die mem reserved-memory, MHUv2 mailbox node, regulators | `E1M-AEN801.yaml` (ospi_memories, hyperram, mailbox, pad_routes) |
| `e1m-evk.dtsi` | carrier: console UART, I²C buses, GPIO expander (TCAL9538), aliases, bootargs, stdout-path | `metadata/boards/e1m-evk.yaml e1m_routes` |
| `e1m-aen801-evk.dts` | product: `#include` SoC dtsi (from BSP) + SoM dtsi + carrier dtsi; model string; memory nodes | composition only |

The Alif **SoC** dtsi is consumed from the BSP/`linux-alif` (not authored).
HW-sensitive node values (HyperRAM timings, exact reserved-memory addresses,
TF-A load addresses) are **TBD/sanitized in public**; the resolved values land in
**alp-sdk-internal** (see §5).

### 4.2 `linux-alif_%.bbappend` — **author** (mirrors `linux-renesas_%.bbappend`)
Feeds the authored DTS files into the `linux-alif` build and selects the carrier
DTB. No kernel source fork.

### 4.3 Machine conf fill-in — **author** (`e1m-aen801-a32.conf`)
Uncomment/fill, sourced from `devkit-e8` + the SoM preset; TBD what isn't grounded:
- `KERNEL_DEVICETREE = "alif/ensemble/e1m/e1m-aen801-evk.dtb"`
- TF-A platform string + carrier memory map — **TBD(alif-hw-config)**, resolved
  copy in alp-sdk-internal.
- `ALP_BOOT_DEVICE = "ospi-nor"` (OSPI0 CS0) — confirm carrier routing.
- keep `xipImage` from the base.

### 4.4 Kernel defconfig delta — **author** (fragment, not a full defconfig)
A small `.cfg` fragment enabling the E1M-EVK peripherals the customer expects
(OSPI NOR+HyperRAM, the carrier I²C/SPI/GPIO controllers, MHUv2/remoteproc for SP3)
**only where the devkit-e8 defconfig doesn't already**. Audit devkit defconfig
first (consume, don't duplicate).

### 4.5 Image recipe — **reuse + a thin XIP profile**
`alp-image-edge.bb` is already generic and machine-driven. SP1 adds an XIP/size
profile (or `IMAGE_*` knobs) so the AEN image fits the OSPI/SRAM footprint —
ideally as machine-conditioned vars in the existing recipe, **not** an AEN-specific
recipe (unification). Decision point flagged in §8.

### 4.6 CI — **author** (one matrix row)
Add `e1m-aen801-a32` to `pr-bitbake.yml`'s machine matrix (config/parse/`do_compile`
to the extent the runner allows), paralleling the V2N/NX9101 rows.

### 4.7 Metadata / generated — **verify, regenerate** (no hand edits)
Confirm `e8.json`, `E1M-AEN801.yaml`, `e1m-evk.yaml` carry everything the above
consumes; extend the metadata (not the generated files) where a field is missing;
regenerate and check the generated-files gate stays clean.

---

## 5. Public / internal split

Per the established pattern (V2N TF-A DDR bbappend lives in alp-sdk-internal):
- **Public `alp-sdk`** gets: the DTS skeletons with sanitized/TBD HW values, the
  bbappend, the machine-conf scaffolding, the kernel `.cfg` fragment, the CI row,
  docs.
- **alp-sdk-internal** gets: the resolved TF-A memory map, HyperRAM timing
  parameters, any schematic-derived carrier pin specifics — as an overlay
  conf-fragment / bbappend the public layer `require`s when present.
- No OSPI part-level timings, schematic nets, or load-address maps in the public
  tree.

---

## 6. What we consume vs author (ADR 0017 tier summary)

| Piece | Tier | Action |
|-------|------|--------|
| `linux-alif` kernel, TF-A, SoC dtsi, `cortexa32` tune, `xipImage`, console, SMP | Tier-1 upstream-native (Alif BSP) | **consume** `require devkit-e8.conf` |
| E1M SoM dtsi / carrier dtsi / product dts | Tier-1 (board overlay only) | **author** |
| `linux-alif` bbappend | — (recipe glue) | **author** |
| machine-conf carrier overrides | — | **author** |
| kernel `.cfg` peripheral fragment | Tier-1 (DT-enable only) | **author** (minimal) |
| `alp_*` A32 peripheral backends | — | **SP2** (consume Linux upstream drivers) |
| MHUv2 / rpmsg multicore | ADR-0017-ADJACENT (MHUv2 driver already in-tree) | **SP3** |

No new Linux drivers in SP1.

---

## 7. TBDs (clean, board/HW-config-gated)

Carried as explicit `TBD(alif-hw-config)`; resolved value (where known) in
alp-sdk-internal:
- TF-A platform string + BL2/BL31/BL33/DTB load addresses.
- HyperRAM timing / reserved-memory exact addresses.
- Confirmation the E1M-EVK routes the same debug UART + boot flash as devkit-e8.
- `cc3501e_otp` helper-firmware path + flash method (already TBD in the preset;
  blocks provisioning, not the bake).
- Final RAM/ROM population per board (default NOR=ROM/HyperRAM=RAM).

---

## 8. Open decision points (resolve during implementation)

1. **Image recipe shape:** machine-conditioned XIP knobs inside `alp-image-edge.bb`
   (preferred — unification) vs a separate `alp-image-edge-xip` profile.
2. **Footprint feasibility:** can a `ros2`-bearing `alp-image-edge` (the conf
   installs `rclcpp sensor-msgs`) fit ~42 MiB RAM + 32 MiB NOR XIP? If not, AEN may
   need a slimmer default `IMAGE_FEATURES` set than V2N — flag, measure during bake.
3. **DTS authored vs generated:** V2N hand-authors DTS; do we keep AEN consistent
   (hand-author) or start moving toward metadata-generated DTS? SP1 keeps parity
   (hand-author) to not expand scope; note as future unification work.

---

## 9. Risks

- **R1 — memory footprint (high).** XIP Linux in ~42 MiB RAM with ROS2 may not
  fit; mitigation: measure early, trim `IMAGE_FEATURES`, XIP rootfs. This is the
  feasibility crux and is testable *without* the board (bake + size report).
- **R2 — devkit-e8 conf not vendored locally (medium).** The exact base values
  (TF-A platform, defconfig contents) aren't in-tree; mitigation: fetch
  `meta-alif-ensemble@scarthgap` into the WSL build tree, read the real base,
  ground the overrides; leave TBD only what's genuinely carrier-specific.
- **R3 — unification drift (medium).** Easy to special-case AEN; mitigation: the
  §2a acceptance lens on every artifact + reuse V2N machinery.

---

## 10. Validation plan (without the board)

| Gate | How | Catches |
|------|-----|---------|
| DTS compiles | `dtc` / kernel DT build of `e1m-aen801-evk.dts` | malformed/missing nodes |
| bitbake parse+configure | `MACHINE=e1m-aen801-a32 bitbake -e alp-image-edge` + `-c configure` (WSL) | conf/layer/recipe errors |
| image bake (footprint) | full bake if feasible; capture `.wic`/size report | R1 footprint |
| generated files clean | `scripts/gen_*` + generated-files gate | metadata/generated drift |
| no-invented-values | grep for non-TBD HW values not traceable to a source | doctrine breach |
| vendor-clean surface | the skill's `git grep` checks | `alif_*` leaking into `<alp/*>` |
| docs/drift | `check_doc_drift.py` | stale docs |

**Deferred to bench (needs board):** TF-A→kernel boot, console, OSPI NOR/HyperRAM
bring-up, peripheral probe (SP2), MHUv2/rpmsg handshake (SP3).

---

## 11. Forward references
- **SP2** — A32 Linux `alp_*` peripheral backends: walk the tier ladder over
  upstream Linux drivers wired by §4.1's carrier DTS; map E1M-EVK peripherals to
  `alp_*` via `src/yocto` wrappers. Builds on SP1's image.
- **SP3** — A32/M55 multicore: A32 releases M55 HP/HE (Zephyr); IPC over the
  MHUv2 mailbox (`E1M-AEN801.yaml mailbox`, ch0 `alp_default_rpmsg`) + shared-mem
  carveout; remoteproc/rpmsg. Parallels the V2N A55+M33 pattern.

---

## 12. Task 0 grounding (2026-06-25) — facts + plan revisions from the real BSP

Grounded against a read-only reference clone of `meta-alif-ensemble@scarthgap`
(`devkit-e8.conf` + `linux-alif_6.12.bb`/`linux-alif.inc` + `trusted-firmware-a.bb`).
Raw numeric memory-map constants are referenced to the BSP / kept in
alp-sdk-internal, not transcribed here (public/internal split, §5).

**Base facts (inherit unchanged):** `cortexa32` tune; kernel `linux-alif` **6.12**,
`KERNEL_IMAGETYPE = xipImage`; defconfig `devkit_e8_defconfig`;
`TF-A_PLATFORM = "devkit_e7"` (shared e7/e8); console `ttyS0@115200` (UART2),
`SMP=1`; SoC-level XIP memory map (`KERNEL_DTB_ADDR`, `XIP_KERNEL_LOAD_ADDR`,
HyperRAM/SRAM/TRUSTED_SRAM bases — see `devkit-e8.conf`).

**Carrier deltas (grounded, landed in `e1m-aen801-a32.conf`):** `MX_FLASH_EN=1` /
`ISSI_FLASH_EN=0` (Macronix MX25UM25645 OctaFlash; triggers `linux-alif`
`do_mx_rev16` xipImage byte-reverse); `ALP_BOOT_DEVICE=ospi-nor` (OSPI0 CS0);
Winbond W958D8NBYA5I HyperRAM on OSPI0 CS1 (confirm `ISSI_HYPERRAM_EN`/
`AP_HYPERRAM_EN` cover OctalRAM generically, else TBD); carrier dtb
`alif/ensemble/e1m/e1m-aen801-evk.dtb`.

**Plan revisions (these change Tasks 2–4 and the bbappend):**
1. **`COMPATIBLE_MACHINE = "(devkit-e).*|(appkit-e).*"`** in `linux-alif` (and the
   TF-A recipe) does NOT match `e1m-aen801-a32` → the `linux-alif`/TF-A bbappends
   MUST extend `COMPATIBLE_MACHINE`. (Plan §4.2 must add this.)
2. **`inherit dct-kernel` + DCT macro DTS** (`DTS_MACRO_FILE`, under `arch/arm`
   aarch32) — the carrier DTS mirrors the Alif **devkit DCT** structure, NOT the
   V2N `arch/arm64` dtsi composition. That structure + `devkit_e8_defconfig` live
   in the **Alif kernel tree** (`ALIF_KERNEL_TREE`), not the meta layer, so
   Tasks 2–4 need the kernel tree on hand to author without inventing.
3. **Multi-layer BSP:** `meta-alif-ensemble` `require`s a base **MSD** layer
   (`ALIF_MSD_BASE`) for `dct-kernel.bbclass`, `ALIF_KERNEL_TREE`/`TFA_TREE`,
   `ospi-config.inc`. A full bitbake build needs poky(scarthgap)+MSD+ensemble+kernel.

**Status:** Task 0 done; the `e1m-aen801-a32.conf` grounded overrides landed
(`KERNEL_DEVICETREE` stays commented pending the DTS). Tasks 2–4 (DTS) + the
bbappend are blocked on the Alif kernel tree; all `bitbake`/bake gates need the
full stack.

### 12a. The decisive gate — Alif kernel + TF-A source (not in the public layers)

Full layer stack (all publicly cloneable): poky-scarthgap + `meta-openembedded`
(oe/python/networking/filesystems) + `meta-poky` + **`meta-alif`** (base/MSD:
`dct-kernel.bbclass`, `tune-cortexa32`, `apss-tiny` distro) + **`meta-alif-ensemble`**
(machine confs, `linux-alif`/TF-A recipes) + `meta-alp-sdk`.

**Where the four vars come from (RESOLVED):** `linux-alif_6.12.bb` and
`trusted-firmware-a.bb` reference `ALIF_KERNEL_TREE`/`TFA_TREE` but the meta layers
never assign them — they are set in `conf/auto.conf` by the Alif **APSS build-setup**
integrator repo (`github.com/alifsemi/alif_linux-apss-build-setup`, branch
`scarthgap_yocto_5.0`). The kernel + TF-A are **public Alif github repos** (no auth
needed; `protocol=https`, optional `HTTPS_USER/PASSWD`):

| var | value (scarthgap) |
|-----|-------------------|
| `ALIF_KERNEL_TREE` | `git://github.com/alifsemi/linux_alif;protocol=https` |
| `ALIF_KERNEL_BRANCH` | `v6.12-dev` |
| `TFA_TREE` | `git://github.com/alifsemi/trusted-firmware-a_alif;protocol=https` |
| `TFA_BRANCH` | `alif_lts-v2.10.8` |

Full layer set (`scripts/fetch-layers.sh`): oe-core + `meta-alif` + `meta-alif-ensemble`
+ `meta-alif-iot` + `meta-yocto`(poky) + `meta-openembedded` + `meta-tensorflow` +
bitbake 2.8, all `scarthgap`, pinned by `REL_TAG` (latest `APSS-v2.2.0`); the setup
generates `auto.conf` with `MACHINE=devkit-e8`, `DISTRO=apss-tiny`. So the build is
fully reproducible from public sources — the earlier "license-gated" read was wrong.

**DCT mechanism:** the carrier config is not a hand-authored dtsi. `dct-kernel`'s
`do_dct_to_dts` reads a **DCT JSON** (`DCT_JSON_FILE`) describing each peripheral's
A32-vs-M55 master assignment and rewrites `<PERIPH>_STATUS "okay"/"disabled"` macros
in the `DTS_MACRO_FILE` header (e.g. `devkit_ex_dct_defines.h`). Both the JSON and
the macro header live in the Alif **kernel tree**. So authoring the E1M-EVK carrier
config = producing the E1M DCT JSON + macro defines against the devkit reference —
which requires the kernel tree. Without it, this is invention (forbidden).

### 12b. DTS authoring recipe (grounded from the unpacked `linux_alif@v6.12-dev`)

Confirmed against the real kernel DTS tree
(`arch/arm/boot/dts/alif/ensemble/{common,devkit}`). The model is flatter than
the V2N SoM/carrier/product split — it is a single product `.dts` per board:

```
e1m-aen801-evk.dts:
  #include "../common/ensemble-ex.dtsi"        // E8 SoC base (Tier-1, consume)
  #include "../e1m/e1m_dct_defines.h"           // OUR defines header (the *_STATUS macros)
  / { model; compatible; aliases{serial0=&uartN};
      reg_3v3/2v7/1v8/1v2 fixed regulators;
      chosen { bootargs = "console=ttyS0,115200n8 root=mtd:physmap-flash.0 rootfstype=cramfs ro"; }; }
  &uartN  { pinctrl; status = UARTn_STATUS; }   // console
  &i2cN   { status = I2Cn_STATUS; <carrier devices from e1m-evk.yaml> }
  &ospi…  / &mem_hyperam { status = MEM_HYPER_STATUS; }  // OSPI NOR + HyperRAM
```

- **Defines header** (`devkit_ex_dct_defines.h` is the reference): every peripheral
  has a `<NAME>_STATUS` macro, default `"disabled"`. Our `e1m_dct_defines.h` sets
  `"okay"` only for the E1M-EVK-populated peripherals (console UART, the carrier
  I²C buses, OSPI/HyperRAM, MHU to the M55s for SP3). **No DCT JSON is needed** —
  `dct-kernel`'s `do_dct_to_dts` only rewrites the header when a `DCT_JSON_FILE`
  exists; absent one, the header defaults apply. (A JSON is the optional runtime
  master-assignment override.)
- **SoC base** `ensemble-ex.dtsi` (47 KB) carries the node labels (`&uartN`,
  `&i2cN`, `&spiN`, `&mem_hyperam`, `&mem_stitch`, the MHU nodes, `&cpu1`) — consume,
  never edit.
- **Build wiring:** the `linux-alif` bbappend installs `e1m-aen801-evk.dts` +
  `e1m_dct_defines.h` into `${S}/arch/arm/boot/dts/alif/ensemble/e1m/`, adds the dtb
  to that dir's `Makefile` (`dtb-$(CONFIG_…) += e1m/e1m-aen801-evk.dtb`), and extends
  `COMPATIBLE_MACHINE` to match `e1m-aen801-a32`. The machine conf's
  `KERNEL_DEVICETREE` then points at `alif/ensemble/e1m/e1m-aen801-evk.dtb`.
  `MACHINE=devkit-e8` already builds end-to-end (env validated, kernel unpacked); the
  carrier introduces only the `e1m/` dir + the COMPATIBLE extension.

**VALIDATED (2026-06-25):** `bitbake -c compile linux-alif` for
`MACHINE=e1m-aen801-a32` BUILDS the carrier dtb
(`arch/arm/boot/dts/alif/ensemble/e1m/e1m-aen801-evk.dtb`, 33 KB), 594/594 tasks,
no board needed. Three integration fixes the bake surfaced (now in the bbappend +
conf): (1) the kernel uses **subdir Makefiles** — `KERNEL_DEVICETREE` alone gives
"no rule to make target", so the bbappend writes `e1m/Makefile` (`dtb-y`) + makes
`ensemble/Makefile` descend into it; (2) `dct-kernel` **force-includes**
`common/devkit_ex_dct_defines.h` via `DTS_MACRO_FILE`, so the carrier dts must
include that same header (not a renamed-guard copy) to avoid `*_STATUS`
redefinition; (3) a `require … # comment` in the machine conf — bitbake `.conf`
splits the `require` line on whitespace, so the `#` parsed as a bogus file
("not a BitBake file"). The baseline = the devkit-e8 peripheral selection; refine
to the E1M-EVK mapping by overriding `*_STATUS` after the include.
- **meta-alp-sdk integration:** its ROS recipes need `meta-ros` (absent from the Alif
  stack) — `BBMASK` `meta-alp-sdk/recipes-ros/` for the AEN build, and the AEN image
  drops ROS2 (the §8.2 footprint call) since ~42 MiB XIP RAM can't host it.

**Build path (now unblocked):** (1) `alif_linux-apss-build-setup` (branch
`scarthgap_yocto_5.0`, `REL_TAG=APSS-v2.2.0`) → `scripts/fetch-layers.sh` clones the
layer stack + bitbake; (2) `source scripts/setup.sh <build>` runs oe-init, adds the
layers, writes `auto.conf` (kernel/TF-A trees + `MACHINE=devkit-e8`); (3) add our
`meta-alp-sdk` layer + switch `MACHINE=e1m-aen801-a32`; (4) `bitbake -c unpack
linux-alif` fetches `linux_alif@v6.12-dev` → read the real DCT defines + devkit DCT
JSON / DTS; (5) author the E1M DCT JSON + carrier DTS + the `linux-alif`/TF-A
bbappend (`COMPATIBLE_MACHINE` extension); (6) `bitbake alif-tiny-image` /
`alp-image-edge`. Validate first against stock `MACHINE=devkit-e8` to confirm the
stack bakes before introducing the carrier.
