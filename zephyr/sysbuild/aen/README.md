@page zephyr_sysbuild_aen_index E1M-AEN sysbuild profile

# zephyr/sysbuild/aen — E1M-AEN secure-boot build profile

Sysbuild configuration template for AEN-Zephyr applications that
want MCUboot-verified secure boot.

## Status

The configuration matches the reference path (MCUboot + ECDSA-P256 +
swap-using-scratch) and builds against the in-repo
`alp_e1m_aen801_m55_{he,hp}` boards (the MRAM partition map it relies on
lives in those board DTs).  The remaining bench-validation step is the
**SES → MCUboot → slot0** chain itself: the E8 bench has proven the
SES → app-direct path (a signed app as the ATOC), and this profile is the
MCUboot-as-ATOC variant of the same flow.  See
[`docs/bring-up-aen.md`](../../../docs/bring-up-aen.md) and
[`docs/aen-provisioning.md`](../../../docs/aen-provisioning.md).

## Boot chain

The Alif Secure Enclave (SES) launches an ATOC image from MRAM.  In this
profile that image is MCUboot, which then verifies and chain-loads the
application from slot0:

```
SES ──ATOC──▶ MCUboot ──verify slot0──▶ application
```

MRAM map (from the board DT; MRAM base `0x80000000`): MCUboot `0x80000000`
· slot0 `0x80010000` (2688 KiB) · slot1 `0x802b0000` (OTA) · scratch ·
storage.  This is also the **SoM-maker provisioning model** — Alp Lab
pre-provisions this MCUboot as the factory ATOC so shipped modules boot
out-of-box and customers `west flash` into slot0 (see
[`docs/aen-provisioning.md`](../../../docs/aen-provisioning.md)).

## Usage

```bash
# From a Zephyr workspace with alp-sdk resolved via west:
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    path/to/app \
    --sysbuild \
    --sysbuild-config alp-sdk/zephyr/sysbuild/aen/sysbuild.conf

# Produces:
#   build/zephyr/zephyr.signed.bin     -- signed application image
#   build/mcuboot/zephyr/zephyr.bin    -- MCUboot bootloader
#
# Flash both (once the module's MCUboot ATOC is provisioned and the SES
# has released the core, so SWD/west flash is available):
west flash --bin-file build/mcuboot/zephyr/zephyr.bin --domain mcuboot
west flash --bin-file build/zephyr/zephyr.signed.bin
```

## Key management

The reference config points at
[`<repo>/keys/mcuboot_dev_ecdsa_p256.pem`](../../../keys/README.md)
-- a **development key**, not for production.  Generate it
locally:

```bash
bash keys/generate_dev_key.sh
```

For production, regenerate the key from a secure source and
hand the public half over to the bootloader build via a
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` override.  The private half
ultimately lives in the OPTIGA Trust M's secure NVM -- see
[`docs/secure-boot.md`](../../../docs/secure-boot.md) for the full
lifecycle.

## Why ECDSA-P256

- Smaller signatures than RSA-2048 (64 bytes vs 256 bytes).
- Mature on Cortex-M55: MbedTLS PSA + nanoecc both support it.
- Matches OPTIGA Trust M's hardware ECC primitive natively,
  so production signing routes through OPTIGA without an
  intermediate key-format conversion.

## Why swap-using-scratch

Trades a small flash partition (scratch slot, typically 16-32
KiB) for crash-robust image swaps: a power loss mid-swap leaves
the device able to recover from the scratch slot on the next
boot.  Alternative modes:

- `overwrite-only`: smaller flash footprint, no rollback.
  Acceptable for non-critical updates; insufficient for
  production OTA.
- `swap-using-move`: doesn't need a scratch partition but uses
  more sector erases per swap (shorter flash life).

Reference path is scratch; consumers override
`SB_CONFIG_MCUBOOT_MODE_*` if their flash budget demands a
different trade-off.
