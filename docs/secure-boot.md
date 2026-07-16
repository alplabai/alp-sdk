# Secure boot on E1M-AEN

This document describes the chain of trust from immutable ROM
through to the application on E1M-AEN-family SoMs, plus the
signing key lifecycle that makes it work.

> **Status: v0.4-prep.**  Scaffolding lands in this revision --
> the sysbuild config, dev-key generation script, and this
> document.  Live compile-verification gates on the
> authoritative [`alp_e1m_evk_aen`](https://github.com/alplabai/alp-zephyr-modules)
> board file landing in the external Zephyr-modules repo.  Until
> then this doc is the contract downstream consumers build
> against.

## Chain of trust

```
┌─────────────────────────────────────────────────────────────┐
│ Alif Ensemble Secure Enclave ROM                            │
│   - Immutable.  Verifies the first-stage bootloader.        │
│   - Roots its trust in an Alif-fab-time-burned public key.  │
├─────────────────────────────────────────────────────────────┤
│ First-stage bootloader (Alif-provided)                      │
│   - Hands off to MCUboot.                                   │
├─────────────────────────────────────────────────────────────┤
│ MCUboot (alp-sdk-built via sysbuild)                        │
│   - Verifies the application image's ECDSA-P256 signature   │
│     against the public key compiled into the bootloader.    │
│   - Failed verification triggers swap-back to the previous  │
│     slot (swap-using-scratch mode).                         │
│   - Public key verification path routes through MbedTLS PSA │
│     once the v0.3.x OPTIGA Trust M PSA driver lands, so HW  │
│     acceleration is transparent.                            │
├─────────────────────────────────────────────────────────────┤
│ Application image                                           │
│   - Signed by the production private key held on the        │
│     air-gapped signing workstation.  (In-chip custody in    │
│     OPTIGA Trust M is the intended end state -- the driver  │
│     is probe-only today; see "Signing key lifecycle".)      │
└─────────────────────────────────────────────────────────────┘
```

The SDK touches the MCUboot layer.  The Alif Secure Enclave
ROM + first-stage are out of scope -- they ship with the SoM
and Alif provides their signing keys.

## Declarative wiring (`boot:` block in `board.yaml`)

The recommended path is the top-level `boot:` block in your
project's `board.yaml` -- the loader (`scripts/alp_orchestrate/`)
emits the matching `SB_CONFIG_*` overlay (sysbuild Kconfig) into
`build/alp_sysbuild.conf` and passes it via
`-DSB_CONF_FILE`.  No hand-edited sysbuild.conf.

```yaml
# board.yaml
boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/prod_ecdsa_p256.pub.pem
  swap_algorithm: scratch
```

See [`docs/board-config-features.md` §Bootloader](board-config-features.md#bootloader-boot----mcuboot)
for the full field reference (including why there is no
`slots:` / `scratch_size_kib:` / `anti_rollback:` field).  Omit the
block to inherit the SDK's stock per-family defaults (AEN-Zephyr:
MCUboot + ECDSA-P256 + swap-using-scratch).  Slot/scratch partition
*sizes* come from the board DT `partitions {}` node, not from
`boot:` -- declare the actual layout via `storage:` if you want it
explicit.

## Signing key lifecycle

### Development

1. Clone the repo.
2. Run `bash keys/generate_dev_key.sh` once.  Generates
   `keys/mcuboot_dev_ecdsa_p256.pem` (gitignored).
3. Build with sysbuild:
   ```bash
   west build -b alp_e1m_evk_aen \
       path/to/app \
       --sysbuild \
       -- -DSB_CONF_FILE=<abs-alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
   ```
   (Or, if your `board.yaml` carries a `boot:` block, the loader's
   emitted overlay at `build/alp_sysbuild.conf` is the canonical
   `-DSB_CONF_FILE` path.)
4. `build/zephyr/zephyr.signed.bin` is your signed image.
5. Flash both the MCUboot bootloader and the signed app:
   ```bash
   west flash --bin-file build/mcuboot/zephyr/zephyr.bin --domain mcuboot
   west flash --bin-file build/zephyr/zephyr.signed.bin
   ```

The dev key has signing power equivalent to "every developer
who's ever cloned the repo".  Never use it in a fielded device.

### Production

> **The OPTIGA Trust M signing path described below is NOT
> implemented.**  The SDK's OPTIGA Trust M driver is
> **probe-only**: `optiga_trust_m_init` reads the I2C_STATE
> register to confirm the part ACKs, and every other entry point
> -- product info, raw APDU transport, `CalcSign`, `GenKeyPair`,
> ECDH -- returns `ALP_ERR_NOSUPPORT` after argument validation
> (`chips/optiga_trust_m/optiga_trust_m.c`; the contract is pinned
> by `tests/scripts/test_optiga_probe_only_contract.py`).  There is
> no key generation, no key export, and no signing through the
> secure element today.  **Do not architect a product's key
> management around it** until the Infineon host-library transport
> is integrated (issue #481).
>
> Use the air-gapped workstation flow below.  The OPTIGA design is
> retained here as the intended end state, clearly marked.

Provisioning happens at SoM manufacturing time -- *before* the
device ships.

#### What to do today: air-gapped signing workstation

1. **Generate the production key on an air-gapped workstation.**
   A machine that never touches the network, holding the ECDSA-P256
   private key on encrypted storage (ideally a smartcard or HSM you
   already trust).  This machine is the key store.
2. **Publish the public half.**  Commit it as
   `keys/mcuboot_prod_ecdsa_p256.pub.pem`; the manufacturing CA
   signs it so downstream tooling can verify provenance.  Only the
   public half ever enters git.
3. **Compile the bootloader against the production public key.**
   Override `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` to point at
   `keys/mcuboot_prod_ecdsa_p256.pub.pem` when building MCUboot for
   production firmware.
4. **Sign release images across the air gap.**  Carry the unsigned
   image to the workstation, sign it there with stock `imgtool`, and
   carry `zephyr.signed.bin` back.  The release pipeline never holds
   signing power.

The security property this gives you: the private key never touches
a networked machine.  Compromise of the dev key, the build host, or
CI does not yield production signing power -- physical access to the
air-gapped workstation does.  That is a weaker guarantee than
in-chip key custody (an operator with access can copy the key), so
treat workstation access control as the control that matters.

#### Intended end state (pending #481)

Once the OPTIGA Trust M host-library transport lands, the private
key is generated *inside* the chip (slot 0xE0F0 is the convention
for the MCUboot signing key) and never leaves its secure NVM; the
public half is read out over I²C, and the release pipeline signs
through a dedicated host with physical access to a provisioned part
(`imgtool` supports external ECDSA signers via
`--public-key-format hash`).  That removes the copy-the-key risk
above.  None of it works yet.

### Key rotation

MCUboot supports compiling against multiple public keys, so rotation
does not depend on the secure element:

1. Generate the next-generation key on the air-gapped workstation.
   (End state, pending #481: provision a new OPTIGA Trust M slot,
   e.g. 0xE0F1.)
2. Compile the bootloader with both `*_pub.pem` files committed
   under `keys/`.  Signed images from either key are accepted.
3. Roll the new key out to fielded devices via OTA (signed by
   the *current* key).
4. Wait one full update window (typically 90 days).
5. Compile the bootloader with only the new key.  Roll out the
   new bootloader.  Old key is now untrusted.

The rotation cadence is policy, not a hard constraint -- the
chain stays intact regardless of cadence as long as no signed
image escapes a slot whose key is still trusted.

## Failure modes + rollback

MCUboot in `swap-using-scratch` mode handles three pathological
cases:

| Failure                                | What happens                                   |
|----------------------------------------|------------------------------------------------|
| Signed image fails verification        | Boot the previous slot.  No state change.      |
| Mid-swap power loss                    | Scratch slot holds the in-flight bytes; next   |
|                                        | boot recovers either to the old or the new     |
|                                        | image atomically.                              |
| Swapped-in image crashes before        | MCUboot's "test" mark stays set; next boot     |
| `boot_set_confirmed()` lands           | swaps back to the previous slot automatically. |

`boot_set_confirmed()` is the application's signal that the
new image is healthy.  Apps that don't call it within a
documented time window get rolled back -- a watchdog-friendly
safety net for OTA.

## See also

- [`zephyr/sysbuild/aen/README.md`](../zephyr/sysbuild/aen/README.md) -- how
  to invoke the sysbuild config.
- [`keys/README.md`](../keys/README.md) -- key files + dev
  key generation.
- [`docs/cc3501e-bridge.md`](cc3501e-bridge.md) -- the CC3501E
  Wi-Fi bridge's role in OTA delivery on AEN.
- [`VERSIONS.md`](../VERSIONS.md) -- versioned roadmap; secure
  boot / OTA shipped together in v0.4.
