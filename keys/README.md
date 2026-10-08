@page keys_index MCUboot signing keys

# keys/ — MCUboot signing keys

**This directory holds development keys for MCUboot image
signing.  Production keys never live in git.**

> **Bench status.** MCUboot's ECDSA-P256 verification of an
> `imgtool`-signed slot0 image is measured working on E1M-AEN801
> (`AE822FA0E5597LS0` Rev A0, alp-sdk `0da1f1b4`) -- see
> [`docs/secure-boot.md`](../docs/secure-boot.md) for the full
> measurement.

## Key files

| File                              | Type                 | Tracked in git? | Notes |
|-----------------------------------|----------------------|-----------------|-------|
| `mcuboot_shared_dev_ecdsa_p256.pem` | ECDSA-P256 dev key (private) | YES, deliberately | The **shared development key** (#2421). Alp Lab's factory MCUboot on pre-provisioned E1M-AEN modules trusts it, and [`zephyr/sysbuild/aen/sysbuild.conf`](../zephyr/sysbuild/aen/sysbuild.conf) signs with it by default, so an image built from a fresh clone boots on such a module with no key setup. **Public: it gives no security.** Never ship a product that trusts it. |
| `mcuboot_dev_ecdsa_p256.pem`      | ECDSA-P256 own key (private) | NO (`.gitignore`'d) | Optional: your own development key, from `generate_dev_key.sh`. Only a module whose MCUboot you re-provisioned with this key will accept images signed by it. |
| `mcuboot_prod_ecdsa_p256.pub.pem` | ECDSA-P256 prod pub  | YES (when it lands) | Public half only.  Compiled into the bootloader for verification.  Production private key never leaves OPTIGA Trust M. |
| `alp_release_signing_ecdsa_p256.pub.pem` | ECDSA-P256 release pub | YES | Public half of the **SoM-release** signing key — a *separate* concern from MCUboot (it signs release bundles / provisioning records, not firmware images).  Used by `scripts/check_som_bundle.py` to verify bundle provenance.  See [`docs/som-release-signing.md`](../docs/som-release-signing.md).  The private half lives in alp-sdk-internal (pilot) / a hardware signer (production), never here. |

## Why a development key is committed

Pre-provisioned modules ship in lifecycle state DM: debug is open and the
module is fully re-provisionable, so nothing on it is secret yet. What a
developer needs on day one is an image the module's MCUboot accepts. A
per-clone random key cannot give that, because the factory MCUboot can only
trust a key that existed when it was built. So the SDK publishes one shared
development key, the factory MCUboot is built with it, and anyone can sign
for it. This is the same model as MCUboot's own published `root-ec-p256.pem`.

The consequence is the point: **anyone can sign images for a module still
running the factory MCUboot.** Before a product ships, build MCUboot with
your own key (below), re-provision it over the SE-UART
([`docs/aen-provisioning.md`](../docs/aen-provisioning.md)), and sign with that key.

## Your own development key

```bash
bash keys/generate_dev_key.sh
```

The script wraps Zephyr's `imgtool` to write an ECDSA-P256 private key to
`keys/mcuboot_dev_ecdsa_p256.pem` (gitignored). Build with
`-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=<abs>/keys/mcuboot_dev_ecdsa_p256.pem`,
and re-provision the module with the MCUboot that build produces: the
factory MCUboot does not trust this key. The matching public key is
extracted by MCUboot's build at compile time, so no separate `.pub.pem` is
needed.

## Production key lifecycle

The production private key is generated and held on an air-gapped
signing workstation; images are signed there with stock `imgtool`
and carried back across the air gap.  The public half is signed by
the Alp Lab manufacturing CA and committed to git as
`mcuboot_prod_ecdsa_p256.pub.pem`.

In-chip custody -- the key generated inside OPTIGA Trust M's
hardware key generator, never leaving the secure NVM -- is the
**intended end state**, not today's flow: the SDK's OPTIGA Trust M
driver is probe-only, and its keygen / export / sign entry points
return `ALP_ERR_NOSUPPORT` (issue #481).  See
[`docs/secure-boot.md`](../docs/secure-boot.md) for the full
provisioning + handover flow.

## Rotating keys

The OTA + secure-boot path supports multi-key MCUboot
configurations -- compile the bootloader with both the current
and next-generation public keys; signed images accepted from
either; once every fielded device is past the rollover window,
compile out the old key.  The cadence is documented in
[`docs/secure-boot.md`](../docs/secure-boot.md).
