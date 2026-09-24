### Added — `provision_som.py plan | run | status`: a step machine that provisions a V2N / V2N-M1 SoM from blank silicon

The flat `provision_som.py --bundle` flow could only plan the V2N writes. The
new subcommands drive a blank module end to end: SCIF download and the Flash
Writer put a transient eMMC-boot BL2 and the FIP into eMMC boot1, U-Boot
boots the release image from the microSD, and Linux then writes xSPI, eMMC
boot1, the eMMC rootfs, the identity EEPROM, the GD32 and the Secure Data
Page. The flow and its hazards are in `docs/provisioning-v2n.md`.

- **Dry run by default.** `plan` and `run` without `--execute` run the
  read-only probes and print every command a step would issue. Every step has
  a read-only probe and counts as done only when the probe passes again after
  the step runs, so a rerun skips finished work.
- **Gates before any write.** The bundle must match its hashes and carry
  `bl2`, `bl2_mmc`, `fip` and `system_image`. The SKU and the DDR tier must
  agree across the CLI, bundle, preset, BL2 images and U-Boot banner. A
  `v2n-m1` FIP must carry the DEEPX rail line, and the dtb U-Boot loads must
  be in the image. The FIP write can never reach the CM33 image region.
- **Identity EEPROM safety.** Only the five known N24S128 `0x58` frames can
  be built, and none of them writes selector `0x06`. The manifest is written
  only to a blank, unlocked array. The Secure Data Page is written and
  verified; locking it is a separate `run --lock` that needs a shippable
  ledger record, a fresh re-read of the page and the serial typed again.
- **Ledger.** Facts go to the private per-unit ledger even when a step fails.
  A unit with the known power-chip register defect, a gate override, or an
  unsigned `--build-dir` image is recorded as not shippable.
- **Bundle schema.** New component role `bl2_mmc` (`emmc:boot1`) and an
  optional `memory_tier` (`dram_mbit`, `label`).
- **Backend rename.** The `xspi_flashwriter` flash backend is now
  `renesas_flashwriter_scif`. It plans xSPI and eMMC boot1 writes and refuses a
  confirmed write. The flat flow reports a `bl2_mmc` component as skipped.

None of the hardware paths have run on a board yet.
