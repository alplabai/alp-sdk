### Added — TF-A init for the E1M-AEN803 S80KS5122 HyperRAM, UNTESTED ON SILICON (#1970)

Alif's TF-A had no init driver for the Infineon/Cypress S80KS5122 HyperRAM on
OSPI0, so the `0xa0000000` window was unusable to Linux. This is written
**build-only**: it has never run against a HyperRAM that answered.

- **New patch** `meta-alp-sdk/recipes-bsp/trusted-firmware-a/trusted-firmware-a/alif-s80ks5122-hyperram-init.patch`
  adds an opt-in BL32 driver (`S80K_HYPERRAM_EN`). The register sequence is
  transcribed from the Alif DFP (`ospi_psram_xip.c` + `S80K_HyperRAM.c`) with
  the DFP `file:line` cited at every write. CR0 (`0x8F2D`), the XIP wait
  cycles (7) and the second-die CR0 write follow the S80KS5122 datasheet,
  because the DFP's single-die values are wrong for this 512 Mbit dual-die part.
- **CK/CK# gate** `S80K_HYPERRAM_CLK_PAIR_CROSSED` (default `1`) names the
  E1M-AEN803 2626-r2 board bug: the pair is crossed at the part, which
  answered a bench bit-bang (`ID0=0x0F86`, `ID1=0x0001`) only with the pair
  inverted, and no firmware lever fixes it. While `1` the driver logs and
  leaves the device untouched. Set `0` for a corrected board (R3, or an r2
  with the R17/R18 rework).
- **E1M-AEN801 is unaffected**: the bbappend wires the patch and the knob for
  `e1m-aen803` only, and the patch is stock-identical when the knob is unset.
  Like the console knobs it sits beside, it is inert until an
  `e1m-aen803-a32` MACHINE exists.
- Verified by `git apply --check` and an `arm-zephyr-eabi` `PLAT=devkit_e7`
  `bl32` build against `alifsemi/trusted-firmware-a_alif` `alif_lts-v2.10.8`;
  **not** run on hardware.
