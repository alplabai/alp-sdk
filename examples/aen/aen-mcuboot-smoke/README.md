# aen-mcuboot-smoke — SES → MCUboot → slot0 boot-chain bench test (E1M-AEN801)

Minimal slot0 application used to bench-validate the **production** secure-boot
chain on the Alif Ensemble E8: `SES ──ATOC──▶ MCUboot ──verify slot0──▶ app`
(the `sysbuild/aen` MCUboot profile). It only proves it BOOTED — it prints to
the RAM console (read over SWD) and reports `VTOR` to show where it runs from.

## Build (sysbuild: MCUboot + signed slot0 app)

```bash
west build -p always --sysbuild \
    -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-mcuboot-smoke -d build/mcuboot-smoke -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
    '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="<abs>/keys/mcuboot_dev_ecdsa_p256.pem"'
# Produces:  build/mcuboot-smoke/mcuboot/zephyr/zephyr.bin        (MCUboot, ITCM-linked)
#            build/mcuboot-smoke/aen-mcuboot-smoke/zephyr/zephyr.signed.bin  (slot0 app)
```

Notes (bench-derived 2026-06-16, first on-silicon bring-up of this chain):
- `sysbuild/mcuboot.overlay` retargets MCUboot to **ITCM**. The Alif SES loads an
  ATOC image to RAM and jumps to it — `app-gen-toc` REJECTS an MRAM `loadAddress`
  ("An MRAM address can not be used in loadAddress attribute"), so MCUboot cannot
  be MRAM-XIP for the ATOC; it must be RAM-linked. slot0/slot1 stay on the MRAM
  flash-controller (`&mram_storage`) for `flash_map`; MCUboot reads/verifies them
  via the `flash_mram_alif` driver (brought in-tree, Tier-2).
- `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` resolves relative to `WEST_TOPDIR`, not the
  repo — pass an absolute path (above) or place the key at `<topdir>/keys/`.

## Flash + validate

```bash
cd <setools>/app-release-exec-linux
# MCUboot as the SES ATOC (ITCM load+boot):
#   app-cfg entry: { binary mcuboot.bin, cpu_id M55_HE, loadAddress 0x58000000,
#                    flags [load,boot], signed true }
./app-gen-toc -f build/config/app-mcuboot-only.json
# <your-serial-device>: your OS's port name for the SE-UART adapter --
# see docs/cross-platform-setup.md §7.7 for the per-OS naming convention.
./app-write-mram -c <your-serial-device> -p
```

**Status (bench, E8): full chain RESULT PASS** (commit `7e3b2c58`, PR #170).
- **SES → MCUboot: VALIDATED.** After flashing the MCUboot-only ATOC, J-Link halt
  shows PC inside MCUboot `main()` (ITCM) — the SES loads + boots the signed,
  ITCM-linked MCUboot.
- **MCUboot → slot0 → app: BOOTS.** With the signed app written to MRAM slot0
  (0x80010000) as a SES-authenticated user image, MCUboot chainloads it and this
  app's banner appears in the RAM console with `VTOR` in slot0-XIP — the full
  `SES → MCUboot → slot0 → app` cascade boots end-to-end. The final bug was
  `do_boot()` using `CONFIG_FLASH_BASE_ADDRESS = 0x0` (ITCM) for the MRAM slot
  vector; the fix is an upstreamable MCUboot `flash_map_extended.c` patch (the
  `alif,mram` controller's `reg = 0x80000000`) carried via `west patch`
  (`zephyr/patches.yml`), plus `+DCACHE=n` and `ROM_START_OFFSET = 0x800`.
- **Trust model = SE root-of-trust.** This chain runs MCUboot as
  `SINGLE_APP + SIGNATURE_TYPE_NONE`: the SES (Alif Secure Enclave) verifies the
  signed slot0 ATOC content cert at the SES stage, and MCUboot acts as the A/B
  chainloader on top. **Software ECDSA verification *inside* MCUboot stays
  blocked** (the separate E8/M55 software-bignum non-convergence) and is not on
  this path — the SE does the cryptographic verify, not the bootloader.
