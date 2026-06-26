# AEN no-board completion — arch-aware DT cells + carrier i2c nodes + M55 firmware verify (A+B+C)

**Date:** 2026-06-26
**Branch:** `feat/aen-a32-yocto-bringup` · **PR:** #264
**Predecessors:** SP1/SP2/SP3 (carrier dtb + image, A32 `alp_*` example, A32↔M55-HP multicore).

Three independent, board-independent completion items batched into one spec. (Item D
— full `alp` DISTRO + `alp-image-edge` — is deferred to its own sub-project; it is
exploratory and needs its own investigation of the Renesas-hardwired `alp.conf`.)

## A — Generator arch-aware DT reserved-memory cells

### Problem
`scripts/alp_orchestrate.py::emit_dts_reservations` hardcodes
`#address-cells = <2>` / `#size-cells = <2>` and a 64-bit base/size split for every
target. SP3 had to **hand-adjust** the generated `aen801-dts-reservations.dtsi` to
32-bit cells because the Ensemble-E8 A32 cluster is AArch32 (the base
`ensemble-ex.dtsi` declares `reserved-memory` with `#address-cells/#size-cells = <1>`).
That hand-edit is a drift trap: the next `--emit` reintroduces the 64-bit form. There
is no arch/address-width signal in the SoC JSON today (the `cores[]` carry no arch).

### Design
- **Metadata:** add a SoC-level field `linux_phys_addr_bits` (integer, `32` or `64`)
  — the physical-address width of the SoC's **Linux-running A-cluster**, which sets the
  reserved-memory cell count (`cells = linux_phys_addr_bits / 32`). Add it to
  `metadata/schemas/soc-spec-v1.schema.json` (`additionalProperties: false`, so the
  property must be declared; enum `[32, 64]`, optional). Set `32` in
  `metadata/socs/alif/ensemble/e8.json` (A32) and `64` in
  `metadata/socs/renesas/rzv2n/n44.json` (A55).
- **Generator:** `emit_dts_reservations` reads `project.soc_spec.get("linux_phys_addr_bits", 64)`
  (default 64 keeps existing behavior for SoCs that haven't declared it), computes
  `cells = bits // 32`, and emits `#address-cells`/`#size-cells = <cells>` plus a
  `reg` with `cells` words for base and `cells` words for size (1-cell:
  `<0xBASE 0xSIZE>`; 2-cell: `<0xHI 0xLO 0xHI 0xLO>`). The blocked-comment path is
  unchanged.
- **Regenerate** `meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi`
  straight from the generator (now correct 32-bit) and **drop** the hand-adjust note +
  the `TODO(arch-aware-cells)` in the generator.

### Validation
- `tests/scripts/test_alp_orchestrate.py`: assert the AEN801 board emits
  `#address-cells = <1>` and a 2-word `reg`; assert a V2N (AArch64) board emits
  `#address-cells = <2>` and a 4-word `reg` (the existing V2N path must be unchanged).
- Re-`bitbake -c compile linux-alif` and decompile to confirm the regenerated dtsi
  still yields the carveout node (the generator output must equal the committed dtsi —
  no residual hand-edit).
- Metadata/schema gates pass (`py -3.14`).

## B — Carrier DTS on-bus i2c sensor nodes

### Problem
The SP1 carrier DTS `&i2c2` (sensor bus) carries a `TODO(e1m-evk-hw)` placeholder for
the on-bus device nodes. The addresses are now grounded in
`include/alp/boards/alp_e1m_evk.h` (board header is the authoritative source).

### Design
Replace the `&i2c2` TODO comment in
`meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` with the device
nodes, addresses transcribed from the board header (nothing invented):

| Node | addr | compatible |
|---|---|---|
| `bmi323@68` | `0x68` (`EVK_I2C_ADDR_BMI323`, U13) | `bosch,bmi323` |
| `icm42670@69` | `0x69` (`EVK_I2C_ADDR_ICM42670`, U12) | `invensense,icm42670p` |
| `bmp581@47` | `0x47` (`EVK_I2C_ADDR_BMP581`, U14) | `bosch,bmp581` |
| `ina236@40..42` + `@49..4B` | `0x40,0x41,0x42,0x49,0x4A,0x4B` (6×, split-bank A/B) | `ti,ina236` |
| `tcal9538@72` | `0x72` (`EVK_I2C_ADDR_TCAL9538`, U35) | `ti,tca9538` |

Each node gets `reg = <0xADDR>;` and its `compatible`. The bus already declares
`#address-cells = <1>; #size-cells = <0>;`. A header comment notes these are
EVK-carrier devices and that nodes whose Alif-kernel driver is absent simply don't
bind (harmless — the SP2 userspace `i2c-dev` path is unaffected).

### Validation
- `bitbake -c compile linux-alif` + decompile shows the six device nodes under the
  i2c2 controller at the right addresses.
- Note in the bake evidence which `compatible`s the booted kernel actually has drivers
  for (deferred to bench; the dtb is correct regardless).

## C — M55-HP firmware Zephyr-SDK rebuild (verification, internal artifact)

### Problem
SP3's M55-HP ELF was built with `gnuarmemb` (system linker script, Zephyr SDK absent),
producing orphan-section warnings for the `alp_backends_*` iterable sections — the
backend-registration mechanism may not place those entries inside
`__alp_backends_start/__alp_backends_end`, so `alp_*` backends could silently fail to
register at runtime.

### Design
- Install the Zephyr SDK in WSL (`west sdk install` / the upstream installer; `west` is
  already present at `~/.local/bin/west`).
- `west build -b alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp
  examples/multicore/rpmsg-aen/m55_hp` with the Zephyr toolchain (not gnuarmemb).
- Verify from the linker `.map` that `__alp_backends_start` … `__alp_backends_end`
  bracket the registered backend entries (i.e. no orphan placement), and that the build
  is warning-clean for the iterable sections.
- The ELF stays **internal** (untracked, `*.elf`-ignored; built out-of-tree, supplied
  by alp-sdk-internal) — **no binary is committed.**

### Committed output
- Update the `aen-m55-hp-fw_0.6.bb` recipe comment + the `rpmsg-aen` README to record
  the **verified** Zephyr-SDK build (replacing the gnuarmemb caveat with the confirmed
  procedure), and record the `.map` evidence (the `__alp_backends_*` bracket) in
  `docs/superpowers/notes/2026-06-25-aen-bsp-grounding.md`.
- If the Zephyr-SDK build surfaces a real source defect in the producer/slice, fix it
  (that fix is committable C under `examples/multicore/rpmsg-aen/m55_hp/`).

### Validation
- The `.map` shows the backend entries inside the iterable section bounds (the concrete
  no-board proof). Runtime execution stays board-gated.

## Global constraints (all items)

- Never invent values — transcribe from the board header / SoC sources, else
  `TODO(...)`. "Alp Lab" spelling; no `Co-Authored-By: Claude` footer.
- `py -3.14` for Python/metadata; clang-format-22 for any C/H (incl. meta-alp-sdk DT
  `.h`); the orchestrator/metadata gates and `check_doc_drift.py` must pass.
- WSL bakes via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`;
  `MACHINE=e1m-aen801-a32`, `DISTRO=apss-tiny`, SP1 `BBMASK` + `BB_DANGLINGAPPENDS_WARNONLY`.
- No binaries in public git (the `*.elf` guard stands; C's ELF stays internal).

## Risks

- **A — V2N regression.** The 64-bit default + explicit `linux_phys_addr_bits: 64` on
  n44 must keep V2N's carveout output byte-identical to today. The test asserts the
  2-cell form; if any V2N consumer hardcodes the old output, surface it.
- **B — absent kernel drivers.** The Alif kernel may lack `bosch,bmi323` etc.; the nodes
  are then inert. Acceptable (documents the bus, dtb valid); not a defect.
- **C — Zephyr SDK install may be large/slow** (~GB). If it cannot be installed in the
  WSL environment, C degrades to BLOCKED — report the env limitation; the gnuarmemb
  caveat stays documented and the rebuild defers to a Zephyr-SDK-equipped host. Do not
  fake the `.map` evidence.
