@page zephyr_sysbuild_aen_index E1M-AEN sysbuild profile

# zephyr/sysbuild/aen — E1M-AEN secure-boot build profile

Sysbuild configuration template for AEN-Zephyr applications that
want MCUboot-verified secure boot.

## Status

The configuration matches the reference path (MCUboot + ECDSA-P256 +
single application slot, `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y`) and
builds against the in-repo `alp_e1m_aen801_m55_{he,hp}` boards (the MRAM
partition map it relies on lives in those board DTs).  Since #1069 the
two boards' slot0s are DISJOINT MRAM windows (HE `0x80010000`, unchanged;
HP `0x802b0000`, moved off the old shared window) so both cores' images
can be resident in App MRAM at the same time -- OTA is deferred (no
secondary/scratch slot) to make that budget fit; see "Why a single
application slot" below and `metadata/e1m_modules/E1M-AEN801.yaml`
`memory_map:`.  The **SES → MCUboot → slot0** boot +
signature-verification chain is measured working on real silicon:
E1M-AEN801 (`AE822FA0E5597LS0` Rev A0), alp-sdk `0da1f1b4`, and this IS
the config that was proven (`CONFIG_SINGLE_APPLICATION_SLOT=y` -- no
swap-mode mismatch, unlike the two-slot swap-using-scratch profile this
file carried before #1069).  SES launches MCUboot from ITCM, MCUboot
verifies slot0 (MRAM XIP) with `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y`
and `CONFIG_BOOT_VALIDATE_SLOT0=y` (read back from the built
`mcuboot/zephyr/.config`, not assumed), and the app boots --
`PC=80012FBC`, `VTOR=80010800`, `CFSR=00000000`, `IPSR=000`.
Verification was proven live, not inferred from a clean boot: flipping
one byte of the TLV `0x22` ECDSA signature (file offset `0x4a30`,
`0xda` -> `0xdb`, TLV `0x10`/SHA-256 and TLV `0x01`/key intact)
produces `D: bootutil_verify_sig: ECDSA builtin key 0` then `E: Unable
to find bootable image`, `VTOR = 0x00000000` -- a halt, not a swap-back
(a single-slot build has no previous slot to swap back to).

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
an `imgtool`-signed image `loadbin`'d to `0x80010000` (the M55-HE
window; unchanged by #1069) is verified by MCUboot and chainloaded, and
survived three cold power-cycles.
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

MRAM map (from the board DT; MRAM base `0x80000000`; disjoint per core
since #1069, see `metadata/e1m_modules/E1M-AEN801.yaml` `memory_map:`):
MCUboot `0x80000000` (64 KiB, shared) · HE slot0 `0x80010000` (2688 KiB,
bench-proven, unchanged) · HP slot0 `0x802b0000` (2688 KiB, moved off
the old shared `0x80010000` window) · reserved `0x80550000` (64 KiB,
ex-scratch, unused -- OTA deferred) · storage `0x80560000` (96 KiB) ·
atoc `0x80578000` (32 KiB, SE-owned -- SETOOLS top-anchors the ATOC
application table there and grows it downward; not customer-writable,
see #1289).
Each per-core board DT only carries ITS OWN slot0 partition entry (plus
the shared mcuboot/reserved/storage/atoc entries) -- the sibling core's slot0
is a disjoint physical window this board never touches.

**Why one shared `mcuboot` window is safe even though each core needs a
differently-linked MCUboot binary:** `metadata/e1m_modules/E1M-AEN801.yaml`
keeps `mcuboot` as a single 64 KiB region (`accessible_from: [m55_he,
m55_hp]`), and both boards' generated DTs declare the same
`boot_partition: partition@0`.  That's benign, not an oversight: MCUboot
is loaded via the ATOC to ITCM (`loadAddress 0x58000000` for the HE
provisioning flow above) and never resident at the MRAM `mcuboot`
address itself -- the `boot_partition` DT node exists to anchor the
`soc-nv-flash` child's offset-0 origin for `_aen_flash_partitions()`
(see `scripts/gen_zephyr_board.py`), not to describe where MCUboot's
code actually executes from.  Each core's own MCUboot build links for
that core's ITCM regardless of what the shared MRAM `mcuboot` entry says,
so the two cores never contend for the region.  Add this to the bench
checklist before flashing HP's MCUboot: confirm the HP MCUboot build
was linked/signed for `cpu_id M55_HP`, not copied from the HE ATOC.

This is also the **SoM-maker provisioning model** — Alp Lab pre-provisions this
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

## Why a single application slot (not swap-using-scratch)

Before #1069 this profile used swap-using-scratch (a secondary/OTA
slot + a scratch partition for crash-robust swaps), matching neither
core's bench-proven boot -- every E8 measurement to date ran
`CONFIG_SINGLE_APPLICATION_SLOT=y` (see "Status" above).  #1069 made
this profile match what's actually proven, and OTA was DEFERRED rather
than kept: giving both M55 cores a swap-sized secondary slot forces
`slot0_HE + slot0_HP = 2688 KiB` each, which doesn't leave room for the
~2.6 MiB NPU MRAM-model budget on top of everything else in the
5632 KiB App MRAM.  The `reserved` region (the ex-scratch 64 KiB) is
kept as unused headroom, not reclaimed, so a future OTA design has
somewhere to land without another memory-map reshuffle.

`overwrite-only` / `swap-using-move` remain available upstream
(`SB_CONFIG_MCUBOOT_MODE_*`) for a consumer that wants OTA on a
single-M55 AEN SKU (aen401/aen601, which kept the stock symmetric
two-slot board DT layout -- see #1069's PR body) and is willing to
size its own flash budget for it; this file's reference path is
single-app-slot.
