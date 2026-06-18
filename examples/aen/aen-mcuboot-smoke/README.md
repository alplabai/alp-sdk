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
./app-write-mram -c /dev/ttyUSB0 -p
```

**Status (bench, E8):**
- **SES → MCUboot: VALIDATED.** After flashing the MCUboot-only ATOC, J-Link halt
  shows PC inside MCUboot `main()` (ITCM) — the SES loads + boots the signed,
  ITCM-linked MCUboot. (Previously bench-pending.)
- **MCUboot → slot0 → app: pending the slot0 image write.** The signed app must be
  written to MRAM slot0 (0x80010000) as a SES-authenticated user image. The
  upstream `alif_flash` west runner does this (`tools-config` registers the image
  paths); reconstructing it by hand (`app-sign-image` / `app-write-mram -i … -a`)
  hit SETOOLS' build-dir path/registration convention. Wire the `alif_flash`
  runner (or complete `tools-config`) to finish this leg; then this app's banner
  appears in the RAM console with `VTOR` in `[0x80010000, 0x802b0000)` (slot0-XIP),
  confirming the full chain.
