@page zephyr_sysbuild_aen_index E1M-AEN sysbuild profile

# zephyr/sysbuild/aen — E1M-AEN secure-boot build profile

Sysbuild configuration template for AEN-Zephyr applications that
want MCUboot-verified secure boot.

## Status

The configuration matches the reference path (MCUboot + ECDSA-P256 +
swap-using-scratch) and builds against the in-repo
`alp_e1m_aen801_m55_{he,hp}` boards (the MRAM partition map it relies on
lives in those board DTs).  The **SES → MCUboot → slot0** boot +
signature-verification chain is now measured working on real silicon:
E1M-AEN801 (`AE822FA0E5597LS0` Rev A0), alp-sdk `0da1f1b4` -- **but the
build that proved verification ran `CONFIG_SINGLE_APPLICATION_SLOT=y`,
not this profile's two-slot swap-using-scratch mode**; a separate run
on this actual reference profile only proved that it boots (below),
not its swap behaviour.  SES launches MCUboot from ITCM, MCUboot
verifies slot0 (MRAM XIP) with `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y`
and `CONFIG_BOOT_VALIDATE_SLOT0=y` (read back from the built
`mcuboot/zephyr/.config`, not assumed), and the app boots --
`PC=80012FBC`, `VTOR=80010800`, `CFSR=00000000`, `IPSR=000`.
Verification was proven live, not inferred from a clean boot: flipping
one byte of the TLV `0x22` ECDSA signature (file offset `0x4a30`,
`0xda` -> `0xdb`, TLV `0x10`/SHA-256 and TLV `0x01`/key intact)
produces `D: bootutil_verify_sig: ECDSA builtin key 0` then `E: Unable
to find bootable image`, `VTOR = 0x00000000` -- a halt, not a swap-back
(that single-slot build has no previous slot).

**Swap-using-scratch itself -- this profile's actual swap mode --
is `[UNTESTED]`.**  The reference-profile build boots and logs
`I: Bootloader chainload address offset: 0x10000`; that is a boot-only
datum from that separate run, not proof of the swap/rollback path (see
"Why swap-using-scratch" below) -- the `slot1` (OTA) and `scratch`
regions in the MRAM map below are correspondingly untested.

The verified backend is **TinyCrypt**
(`CONFIG_BOOT_ECDSA_TINYCRYPT=y`), not MbedTLS PSA; the `.config` also
confirms `CONFIG_SINGLE_APPLICATION_SLOT=y` and
`CONFIG_FLASH_BASE_ADDRESS=0x0` (MCUboot itself is ITCM-linked).
Still required: `CONFIG_DCACHE=n` (a separate, established hang in
`SCB_EnableDCache`), the board's `ROM_START_OFFSET=0x800`, and
the `zephyr/patches/mcuboot` `do_boot` flash-base patch (a candidate
for upstreaming rather than carrying indefinitely).

**The customer path is now proven too.**  Writing slot0 with a plain
J-Link -- no SETOOLS, no ATOC, no SE-UART -- is measured working:
an `imgtool`-signed image `loadbin`'d to `0x80010000` is verified by
MCUboot and chainloaded, and survived three cold power-cycles.
**Proven at `0x80010000` only** -- writing the ATOC region or erasing
MCUboot itself was not tested.  A tampered signature or a non-MCUboot
image at slot0 both leave the debug port alive (a bad slot0 write
does not brick J-Link access).  Single-slot result
(`CONFIG_SINGLE_APPLICATION_SLOT=y`) -- A/B swap and OTA are untested,
not an upgrade-path guarantee.  Full recipe:
[`docs/aen-provisioning.md`](../../../docs/aen-provisioning.md) §0.5.
See also [`docs/bring-up-aen.md`](../../../docs/bring-up-aen.md).

## Boot chain

The Alif Secure Enclave (SES) launches an ATOC image from MRAM.  In this
profile that image is MCUboot, which then verifies and chain-loads the
application from slot0:

```
SES ──ATOC──▶ MCUboot ──verify slot0──▶ application
```

MRAM map (from the board DT; MRAM base `0x80000000`): MCUboot `0x80000000`
· slot0 `0x80010000` (2688 KiB, bench-proven) · slot1 `0x802b0000`
(OTA, `[UNTESTED]`) · scratch (`[UNTESTED]`) · storage.  This is also
the **SoM-maker provisioning model** — Alp Lab pre-provisions this
MCUboot as the factory ATOC, once per module, over the SE-UART:

```bash
cd "$SETOOLS_DIR"
./app-gen-toc -f build/config/app-mcuboot-only.json
./app-write-mram -c $SE_UART -p
```

(MCUboot entry: `cpu_id M55_HE`, `loadAddress 0x58000000`,
`flags ["load","boot"]`, `signed true`.  The SES banner then shows
`| MCUBOOT- | M55-HE | ... | uLVB |` -- slot0 is no longer an SES boot
entry; MCUboot owns it from here.)  Shipped modules then boot
out-of-box, and customers load apps into slot0 via `west flash` **or**
a plain J-Link with no SETOOLS/SE-UART of their own (see
[`docs/aen-provisioning.md`](../../../docs/aen-provisioning.md) §0.5).

## Usage

```bash
# From a Zephyr workspace with alp-sdk resolved via west:
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    path/to/app \
    --sysbuild \
    -- -DSB_CONF_FILE=/abs/path/to/alp-sdk/zephyr/sysbuild/aen/sysbuild.conf

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

## Why swap-using-scratch `[UNTESTED]`

Neither bench session exercised this mode (both ran
`CONFIG_SINGLE_APPLICATION_SLOT=y`); this section is the documented
design rationale, not a bench-run result.

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
