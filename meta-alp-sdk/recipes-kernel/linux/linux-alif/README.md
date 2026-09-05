# A32 Linux board files for the Alif Ensemble E8 (E1M-AEN SoMs)

Device trees and kernel config fragments for the Cortex-A32 cluster of the
Alif Ensemble E8 (AE822) on Alp E1M-AEN modules.

The device trees are hand-authored on purpose: `scripts/gen_zephyr_board.py`
emits Zephyr board trees only, and Linux board trees stay hand-written in this
repo — the same rule the V2N boards follow.

## Device trees

### Layering

Same model as `../linux-renesas/` — SoC dtsi -> carrier dtsi -> per-board dts
-> named dtb, **not** a patch pile against the vendor reference dts:

| file | layer | holds |
| --- | --- | --- |
| `ensemble-ex.dtsi` (**vendor**, not in this repo) | SoC | every Ensemble peripheral node, all `status = "disabled"` by default |
| `e1m-aen-evk.dtsi` | carrier | the E1M-AEN EVK deltas: console UART, `chosen`/`bootargs`, kernel RAM |
| `e1m-aen803-evk.dts` | board | model string + the SKU delta (A32 unicore) -> `e1m-aen803-evk.dtb` |

A second AEN SKU on the same carrier (E1M-AEN801/E7, `#1966`) is a new dts of
about thirty lines that includes the same carrier dtsi — it must not become a
second copy of the carrier.

### Where the vendor dtsi comes from

`../common/ensemble-ex.dtsi` and `../common/devkit_ex_dct_defines.h` are
**not** in this repo. They ship in the Alif kernel fork:

    github.com/alifsemi/linux_alif, branch v6.12-dev
    arch/arm/boot/dts/alif/ensemble/common/

(branch `main` of that repo is an empty placeholder). The relative include
`../common/ensemble-ex.dtsi` means these two files must be installed into a
directory one level under `arch/arm/boot/dts/alif/ensemble/` — e.g.
`arch/arm/boot/dts/alif/ensemble/e1m/`. That directory also needs a `Makefile`
with a `dtb-$(CONFIG_ARCH_ENSEMBLE)` entry and a `subdir-y += e1m` in the
parent, because the vendor tree has no `e1m` directory.

Note a naming mismatch to resolve when the machine conf is wired up: the
commented-out `KERNEL_DEVICETREE` in `conf/machine/e1m-aen801-a32.conf` names
`alif/ensemble/e1m/e1m-aen801.dtb`. The directory matches, the filename does
not — nothing in the tree produces `e1m-aen801.dtb`; these sources produce
`e1m-aen803-evk.dtb`. Whichever is correct, the two must be made to agree.

Since the dts pin the vendor SoC dtsi and its `PIN_*` / `*_STATUS` macros,
they are **not** portable across `linux_alif` branches.

## Kernel config fragments

Two `.cfg` fragments recording the kernel deltas an actual A32 Linux boot
needed on top of Alif's `arch/arm/configs/devkit_e8_unicore_defconfig`:

    userspace-sysfs-devtmpfs.cfg   CONFIG_SYSFS=y, CONFIG_DEVTMPFS_MOUNT=y
    mram-physmap-atoc-guard.cfg    CONFIG_MTD_PHYSMAP_LEN=0x1FA000

Each carries its own measured symptom in its header comment.

### Why two files and not one

They answer two different questions, and only one of them is dangerous.

* `userspace-sysfs-devtmpfs.cfg` is "make userspace work". Both symbols are
  recoverable mistakes: get them wrong and the shell comes up crippled (no
  `/sys`, no `/dev` nodes), you notice immediately, and you fix it on the next
  flash. They belong together because they share one motivation and one blast
  radius.

* `mram-physmap-atoc-guard.cfg` is "do not corrupt the signed ATOC".
  `CONFIG_MTD_PHYSMAP_LEN` is not a feature switch; it is a bound on a
  writable MTD device that the defconfig value (`0x200000`) runs **22288
  bytes into** the Secure-Enclave-signed `AppTocPackage.bin` at `0x8057a8f0`.
  Nothing warns you: the boot is identical either way until something writes
  the tail of `mtd0`. A separate file keeps that reasoning attached to the one
  symbol it constrains, and makes a "just bump the LEN, we need more rootfs"
  diff impossible to review without reading the layout table.

## What was observed on silicon, 2026-09-05

Linux 6.12.6 boots to an interactive shell on two E1M-AEN803 modules. Chain:
Secure Enclave -> TF-A BL32 (SP_MIN, AArch32) -> Linux as BL33 — no
BL1/BL2/BL31 and no U-Boot. Kernel XIP from MRAM; kernel RAM in on-chip SRAM
(7528 kB).

TF-A is `github.com/alifsemi/trusted-firmware-a_alif`, branch
`alif_lts-v2.10.8`, `PLAT=devkit_e7` with `ALIF_SOC_E8=1` (the default), which
covers both E7 and E8. It hands the dtb to the kernel in `r2` via
`ARM_PRELOADED_DTB_BASE`, so the Secure Enclave never inspects the dtb — it is
not described by the ATOC, and a SETOOLS-only flash cannot place it.

## Status — read this before citing anything above as proven

**The dtb compiles, and the boot it describes happened, but not from these
files in this form.** What booted was an ~805-line working copy of Alif's
`devkit-e8-unicore.dts` with three edits applied in place. The dts/dtsi pair
here distils those same three deltas into the layered shape the repo wants:
`e1m-aen803-evk.dtb` was built with the in-tree `scripts/dtc` of a `linux_alif`
v6.12-dev checkout and the decompiled output checked node-by-node against the
dtb that booted — but this form has not itself been booted. Do not upgrade that
to "silicon-observed" until a board has run it.

The one dtc warning (`/memory1@2000000: duplicate unit-address`) comes from the
vendor `ensemble-ex.dtsi` and is reproduced identically by the vendor's own
`devkit-e8-unicore.dts`.

Also not done, and nothing here should be read as claiming otherwise:

- **No recipe and no bbappend.** There is no `linux-alif` `.bb` or
  `.bbappend` installing these into a kernel tree, and no bitbake or Yocto
  build has ever been run for an AEN machine. `MACHINE = "e1m-aen801-a32"`
  remains a non-buildable placeholder (`#1968` missing `devkit-e8.conf`,
  `#1971` zeus-era layer vs Scarthgap), and there is no AEN803 machine at all
  (`#1966`). These files are the reviewable record of the boot deltas; wiring
  them into a recipe waits on the Scarthgap rehost (`#1972`).
- **No MTD node.** `root=mtd:physmap-flash.0` in the bootargs resolves against
  the MTD physmap over MRAM. That window comes from the kernel config
  (`CONFIG_MTD_PHYSMAP_START` / `_LEN`), not from this dtb.
- **Only the boot path is exercised.** Console, kernel RAM and the root
  cmdline were observed on silicon. Every other peripheral is left at its
  vendor default; nothing else on this carrier has been measured under Linux.

### Unresolved: `/proc/mtd` disagrees with the physmap window

On target `/proc/mtd` reports

    mtd0: 00100000 00100000

i.e. 1 MiB, while the same kernel printed the physmap window as `0x1FA000`
(`physmap platform flash device: [mem 0x80380000-0x80579fff]`). Those two
figures disagree and this note does not settle which is authoritative —
neither has been traced back to its source.

It is benign today: the cramfs is exactly 1 MiB and both figures stay below
`AppTocPackage.bin` at `0x8057a8f0`. It must be settled before anyone uses the
tail of that region (a larger rootfs, a second partition, or any write path),
because the two numbers imply two different answers for where that region ends.
