# sysbuild/aen — E1M-AEN secure-boot build profile

Sysbuild configuration template for AEN-Zephyr applications that
want MCUboot-verified secure boot.

## Status

**v0.4-prep.**  The configuration is complete and matches the
intended v0.4 reference path (MCUboot + ECDSA-P256 +
swap-using-scratch).  The authoritative `alp_e1m_evk_aen` Zephyr
board file lives at
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
(TBD per [`PLAN.md` §6 item 8](../../PLAN.md)).  Once that lands,
the `pr-twister` workflow gains a sysbuild scenario that
compile-verifies this config against a smoke example.  Until
then this directory documents the target so downstream consumers
don't have to reverse-engineer it.

## Usage

```bash
# From a Zephyr workspace with alp-sdk + alp-zephyr-modules
# resolved via west:
west build -b alp_e1m_evk_aen \
    path/to/app \
    --sysbuild \
    --sysbuild-config alp-sdk/sysbuild/aen/sysbuild.conf

# Produces:
#   build/zephyr/zephyr.signed.bin     -- signed application image
#   build/mcuboot/zephyr/zephyr.bin    -- MCUboot bootloader
#
# Flash both:
west flash --bin-file build/mcuboot/zephyr/zephyr.bin --domain mcuboot
west flash --bin-file build/zephyr/zephyr.signed.bin
```

## Key management

The reference config points at
[`<repo>/keys/mcuboot_dev_ecdsa_p256.pem`](../../keys/README.md)
-- a **development key**, not for production.  Generate it
locally:

```bash
bash keys/generate_dev_key.sh
```

For production, regenerate the key from a secure source and
hand the public half over to the bootloader build via a
`SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` override.  The private half
ultimately lives in the OPTIGA Trust M's secure NVM -- see
[`docs/secure-boot.md`](../../docs/secure-boot.md) for the full
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
