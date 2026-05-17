@page keys_index MCUboot signing keys

# keys/ — MCUboot signing keys

**This directory holds development keys for MCUboot image
signing.  Production keys never live in git.**

## Key files

| File                              | Type                 | Tracked in git? | Notes |
|-----------------------------------|----------------------|-----------------|-------|
| `mcuboot_dev_ecdsa_p256.pem`      | ECDSA-P256 dev key   | NO (`.gitignore`'d) | Generated locally via `generate_dev_key.sh`.  Used by [`zephyr/sysbuild/aen/sysbuild.conf`](../zephyr/sysbuild/aen/sysbuild.conf).  **Insecure: signing power equals every developer who's ever cloned this repo.** |
| `mcuboot_prod_ecdsa_p256.pub.pem` | ECDSA-P256 prod pub  | YES (when it lands) | Public half only.  Compiled into the bootloader for verification.  Production private key never leaves OPTIGA Trust M. |

## Generating the dev key

Run once per workspace clone:

```bash
bash keys/generate_dev_key.sh
```

The script wraps Zephyr's `imgtool` (already in the Zephyr Python
requirements) to emit an ECDSA-P256 private key at
`keys/mcuboot_dev_ecdsa_p256.pem`.  The matching public key is
extracted by MCUboot's build at compile time -- no separate
`.pub.pem` is required for the dev workflow.

## Why the dev key isn't in git

Anyone who can sign an image with this key can convince a
device running the matching bootloader that their image is
trusted.  Putting the dev key in git means every clone of the
repo gains that signing power.  For local development that's
acceptable; for any deployment beyond a developer bench it
isn't.

CI workflows generate a per-run key inside the runner so the
artefact never lives on disk longer than the workflow run.

## Production key lifecycle

The production private key is generated inside OPTIGA Trust M's
hardware key generator at SoM provisioning time and never leaves
the secure NVM.  The public half is exported, signed by the ALP
Lab manufacturing CA, and committed to git as
`mcuboot_prod_ecdsa_p256.pub.pem`.  See
[`docs/secure-boot.md`](../docs/secure-boot.md) for the full
provisioning + handover flow.

## Rotating keys

The OTA + secure-boot path supports multi-key MCUboot
configurations -- compile the bootloader with both the current
and next-generation public keys; signed images accepted from
either; once every fielded device is past the rollover window,
compile out the old key.  The cadence is documented in
[`docs/secure-boot.md`](../docs/secure-boot.md).
