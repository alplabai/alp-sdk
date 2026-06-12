# Verifying SoM-release provenance signatures

Alp signs each SoM-release **bundle** (and each per-unit provisioning record) with an
ECDSA-P256 **release-signing key**, so you can verify that a bundle was produced by Alp
and has not been altered in transit or storage. This is *supply-chain provenance* — it is
**not** the device secure-boot chain (see [secure-boot.md](secure-boot.md), which signs
bootable firmware images with a different, device-scoped key).

## Trust model

- The release-signing key is an ECDSA P-256 key. Alp holds the **private** half (in the
  pilot phase a restricted key file; in production a hardware-backed signer). The
  **public** half is published in this repo at
  [`keys/alp_release_signing_ecdsa_p256.pub.pem`](../keys/alp_release_signing_ecdsa_p256.pub.pem)
  so anyone can verify.
- A signature is computed over the bundle manifest serialized in a canonical form (the
  document with its `signature` field set to `null`, sorted keys, compact separators),
  digested with SHA-256. The bundle manifest already records the SHA-256 of every
  component (`bl2`/`fip`/`system_image`) and of the private repro-pin
  (`provenance_ref.sha256`), so one signature over the manifest transitively attests the
  whole release.
- The signature is embedded in the manifest's `signature` field:

  ```json
  "signature": {
    "format_version": 1,
    "algorithm": "ecdsa-p256-sha256",
    "key_id": "0123456789abcdef",
    "signed_at": "2026-06-09",
    "value": "<base64 DER ECDSA signature>"
  }
  ```

  `key_id` is the first 16 hex chars of `SHA-256(DER(public key))` — a lookup hint that
  lets verification match the right key and support rotation.

## Verifying a bundle

```bash
# verify a release bundle against the published public key (the default)
python scripts/check_som_bundle.py \
    --bundle releases/E1M-V2N101/som-0.2.0/bundle.json \
    --require-signature
```

- Without `--require-signature`, an **unsigned** bundle (signature `null`) still passes —
  early bundles such as `som-0.1.0` predate signing.
- `--require-signature` makes an unsigned bundle a failure.
- `--pubkey <PEM>` verifies against a specific public key instead of the published default
  (e.g. during a key rotation, or to check a bundle signed with a not-yet-default key).
- A present-but-invalid signature, a tampered manifest, or an unloadable key all fail with
  a clear `FAIL …` line and a non-zero exit code.

You can also verify programmatically:

```python
import som_signing  # scripts/som_signing.py — verify-only, no private key
pub = som_signing.load_public_key_file("keys/alp_release_signing_ecdsa_p256.pub.pem")
assert som_signing.verify_signature(bundle_dict, pub)
```

## Key rotation

`scripts/som_signing.py` exposes `verify_with_keys(doc, [pub1, pub2, …])`, which accepts a
bundle signed by **any** trusted public key. A rotation is therefore: publish the new
public key, sign new releases with the new key, and retire the old public key after the
overlap window. Already-shipped bundles stay verifiable against the retained old key.

## What this is *not*

This signs **release artifacts** (provenance/traceability), on a release host, across all
SKUs. It is independent of the per-device firmware-image signing in
[secure-boot.md](secure-boot.md) (MCUboot + OPTIGA Trust M, Alif-Ensemble-only): different
key, different surface, different scope.
