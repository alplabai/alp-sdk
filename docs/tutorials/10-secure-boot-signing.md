<!-- Last verified: 2026-05-18 against slice-3b state. -->

# Tutorial 10: Secure boot + image signing

**Target audience:** firmware engineers preparing the first
production build that needs MCUboot-verified secure boot on
AEN-Zephyr.

**Prerequisites:** Tutorial [01](01-first-build.md) completed.
A working AEN EVK or a real E1M-AEN module.

**Outcome:** a signed firmware image that the on-module
MCUboot bootloader verifies before handing off to the
application.  Understand the dev-key flow, the production-key
lifecycle, and the swap-using-scratch rollback path.

**Time:** 30 minutes (after the EVK is on the bench).

> **Companion docs.** [`docs/secure-boot.md`](../secure-boot.md)
> is the design + lifecycle.  This tutorial is the
> step-by-step.  [`docs/security-advisories.md`](../security-advisories.md)
> is the disclosure flow for any signing-related issues you find.

---

## What "secure boot" means here

The on-module MCUboot bootloader verifies every firmware image's
ECDSA-P256 signature against a public key compiled into the
bootloader itself.  Unsigned or wrongly-signed images get
rejected; MCUboot falls back to the previous-known-good slot
(swap-using-scratch mode -- see [ADR 0006](../adr/0006-secure-boot-secure-ota.md)).

The trust chain:

```
  Production signing key (ECDSA-P256 private)
       │  (lives in OPTIGA Trust M secure NVM; never on a host)
       ▼
  Application image                  ┐
  → ECDSA-P256-signed                │  every release
  → ECDSA-P256 pub key in MCUboot    │
       │                              │
       ▼                              │
  MCUboot verifies sig at every boot ┘
       │
       ▼
  Swap-using-scratch on success / rollback on fail
       │
       ▼
  Application runs from primary slot
```

For development / bring-up the dev key (under `keys/`) signs
images.  Production key never enters this dir.

## 1. Generate the dev key (one-time)

```bash
cd ~/work/alp-sdk
bash keys/generate_dev_key.sh
```

Expected output:

```
[generate_dev_key] writing keys/mcuboot_dev_ecdsa_p256.pem (chmod 600)
[generate_dev_key] Done.  Reference from zephyr/sysbuild/aen/sysbuild.conf already points here.
```

The script is idempotent -- re-running it preserves the key if
it exists.  The key is `.gitignored`; never commit it.

> **Production-key flow** (for reference, not in this tutorial):
>
> Production keys are generated **inside the OPTIGA Trust M**
> via Infineon's provisioning flow.  The private half never
> leaves the secure element.  The public half is extracted,
> embedded in the bootloader build via
> `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`, and burned to flash at
> production time.  Full procedure in
> [`docs/secure-boot.md`](../secure-boot.md) "Production key
> lifecycle".

## 2. Build the bootloader + signed app image

The MCUboot profile lives at
[`zephyr/sysbuild/aen/sysbuild.conf`](../../zephyr/sysbuild/aen/sysbuild.conf).
Sysbuild is Zephyr's umbrella build for multi-image projects --
it builds the bootloader + the application in one invocation.

For projects that want to customise the bootloader's signing
key / slot sizes / swap algorithm, drop a `boot:` block into your
`board.yaml`:

```yaml
# board.yaml
boot:
  method: mcuboot
  signing:
    algorithm: ecdsa_p256
    key_file: keys/prod_ecdsa_p256.pub.pem
  swap_algorithm: scratch
```

(Slot/scratch partition *sizes* aren't a `boot:` field -- MCUboot
takes its geometry from the board DT `partitions {}` node; declare an
explicit layout via `storage:` if you need one in your `board.yaml`.)

The loader emits the matching `SB_CONFIG_*` overlay; `west alp-build`
passes it as `--sysbuild-config build/alp_sysbuild.conf`
automatically.  Without a `boot:` block the SDK's stock profile
above is used unchanged.

Direct build (without `west alp-build` orchestration):

```bash
west build -b alif_e7_dk_rtss_he \
    examples/peripheral-io/gpio-button-led \
    --sysbuild \
    --sysbuild-config alp-sdk/zephyr/sysbuild/aen/sysbuild.conf
```

Output artefacts under `build/`:

```
build/
├── mcuboot/zephyr/
│   └── zephyr.bin            # MCUboot bootloader binary
├── zephyr/
│   ├── zephyr.bin             # raw application (don't flash this)
│   └── zephyr.signed.bin      # ECDSA-P256-signed application
```

Flash both:

```bash
west flash --bin-file build/mcuboot/zephyr/zephyr.bin --domain mcuboot
west flash --bin-file build/zephyr/zephyr.signed.bin
```

On the UART you'll see the MCUboot banner followed by the
application's `[gpio] init` banner -- the bootloader verified
the signature, accepted the image, and handed off.

## 3. Verify rejection of an unsigned image

Sign with the wrong key, watch MCUboot reject:

```bash
# Sign with a fake key (generated for this test only).
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
    -out /tmp/fake_key.pem

imgtool sign \
    --key /tmp/fake_key.pem \
    --version 0.1.0 \
    --header-size 0x200 \
    --align 8 \
    --slot-size 0x40000 \
    --pad-header \
    build/zephyr/zephyr.bin \
    /tmp/zephyr.fakesigned.bin

west flash --bin-file /tmp/zephyr.fakesigned.bin
```

MCUboot output on the UART:

```
*** Booting MCUboot v1.x ...
[INF] Starting bootloader
[INF] Image index: 0, Swap type: none
[ERR] Image in the primary slot is not valid!
[ERR] Unable to find bootable image
```

The bootloader refuses to chain into a wrongly-signed image
and halts.  Recovery: reflash with a properly-signed image.

## 4. Verify swap-using-scratch rollback

Two images, both signed, the new one buggy:

```bash
# Build a "buggy" variant that panics shortly after boot.
# In practice: an app that calls k_panic() at start.
west alp-build -b alif_e7_dk_rtss_he examples/buggy-app
imgtool sign \
    --key keys/mcuboot_dev_ecdsa_p256.pem \
    --version 0.2.0 \
    ...
    build/zephyr/zephyr.bin \
    build/zephyr/zephyr-bad.signed.bin

# Stage in secondary slot (offset depends on flash layout):
west flash --bin-file build/zephyr/zephyr-bad.signed.bin --slot 1
# Mark for swap-on-next-boot:
imgtool boot --confirm  build/zephyr/zephyr-bad.signed.bin

# Reboot.
```

MCUboot output:

```
[INF] Image index: 0, Swap type: test
[INF] Starting swap using scratch algorithm
[INF] Boot source: primary slot
[INF] Swap type: test
...
*** [PANIC] ...
*** REBOOT
[INF] Image index: 0, Swap type: revert
[INF] Starting swap using scratch algorithm
[INF] Boot source: primary slot (previous good)
```

The application panics → next boot detects the unconfirmed
swap-test → MCUboot reverts to the previous slot.  Recovery
is automatic; no host intervention needed.

## 5. What this guarantees + doesn't

**Does guarantee:**

- An attacker who can reflash the application slot but doesn't
  have the signing key can't get the new image to boot.
- A buggy-but-signed update reverts cleanly on the next boot
  cycle if the watchdog or panic fires.
- A mid-swap power loss doesn't brick the device --
  swap-using-scratch's atomic-commit semantics handle it.

**Doesn't guarantee:**

- Protection against an attacker with the **signing key itself**
  (production key in OPTIGA mitigates this; dev key in `keys/`
  does not).
- Protection against an attacker with JTAG/SWD access to the
  bootloader region (debug-disable fuse is the production
  mitigation -- see [`docs/bring-up-v2n.md`](../bring-up-v2n.md)
  "Production lock-down").
- Protection against silicon-level side-channel attacks (see
  [`docs/threat-model.md`](../threat-model.md) §5 out-of-scope).

## 6. Production checklist

Before tagging a customer-facing release:

- [ ] Production key generated **inside** OPTIGA Trust M
      (never on a host).  Procedure in `docs/secure-boot.md`.
- [ ] Production public key embedded in the bootloader build
      via `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` -- not the dev
      key.
- [ ] Debug-disable fuse blown.  See per-SoM bring-up doc.
- [ ] Production manifest written to the EEPROM via
      `scripts/program_eeprom.py` (real serial, real
      mfg-date).  See [Tutorial 13: EEPROM
      provisioning](13-eeprom-provisioning.md).
- [ ] Signed image SHA-256 / SHA-512 captured in the release
      artefact list (the `release.yml` workflow does this
      automatically).
- [ ] Mender artefact built + signed against the same root key
      so OTA updates verify (TBD until Mender Zephyr client
      lands per [ADR 0009](../adr/0009-mender-zephyr-client-deferred.md);
      Yocto-side already in place).

## See also

- [`docs/secure-boot.md`](../secure-boot.md) -- design +
  lifecycle.
- [`docs/ota.md`](../ota.md) -- OTA design (Yocto side ready
  for v0.4; Zephyr side deferred to v1.1).
- [`docs/threat-model.md`](../threat-model.md) -- what secure
  boot does + doesn't protect against.
- [`zephyr/sysbuild/aen/sysbuild.conf`](../../zephyr/sysbuild/aen/sysbuild.conf)
  -- the MCUboot profile.
- [`keys/`](../../keys/) -- dev key generator + production
  pub-key bundle location.
