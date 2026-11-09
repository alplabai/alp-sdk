# RZ/V2N security: secure boot, OP-TEE and the Secure IP (N48 only)

Opt-in glue between `meta-alp-sdk` and the Renesas **RZ MPU Security Package
for RZ/V2N AI SDK** (package v4.2.0.1, targets AI SDK v6.30, our BSP). The
package is licence-gated and obtained from Renesas under your own account
(search "RZ MPU Security Package (Linux OS)" on renesas.com); Alp Lab does
not redistribute it and nothing from it is in this repository. Status:
**build-verified on a stand-in MACHINE only, never run on silicon.**

## What it gives you

| Feature | Where it comes from | Notes |
|---|---|---|
| Secure boot (ROM verifies BL2; BL2 verifies BL31, BL32, BL33 with ECDSA P-256 / SHA-256 certificates, optional AES-128-CBC image encryption) | package TF-A flags + `firmware-signing` recipe | needs the OTP provisioned, see below |
| Trusted execution (TF-A BL31 `opteed` + OP-TEE OS BL32 + `tee-supplicant` / `libteec` in Linux) | package `optee-*` recipes | `ENABLE_SPD_OPTEE = "1"` |
| Hardware crypto (Renesas Secure IP: AES-128/256, RSA, ECC P-192/224/256, SHA-224/256, CMAC, TRNG, key wrapping under a per-device key) | OP-TEE driver + `libsecureip` | `ENABLE_RZ_SCE = "1"`; reached only through the package's pseudo-TA and `rz_optee_ta_sce` sample, **not** through OpenSSL, PKCS#11 or the TEE Cryptographic API |
| Anti-rollback, secure debug (JTAG authentication) | ROM, set by provisioning | no build-time part |

There is no OpenSSL engine/provider and no PKCS#11 module: the Secure IP only
accepts wrapped keys, which the standard TEE crypto API cannot carry. Apps
that need it write a Trusted Application against the package's pseudo-TA.
The A55 Arm crypto extensions (`aes`/`sha`/`pmull`) are a separate, kernel-only
feature of the same parts.

## N48 only

The Secure IP exists only on the security parts (`R9A09G056N45GBG` .. `N48GBG`;
`E1M-V2N104..106` / `E1M-V2M104..106` carry the N48). The N44 behind
`E1M-V2N101..103` / `E1M-V2M101..103` is a Renesas *normal device*: the
security features are fused off at manufacture and cannot be enabled.
The glue therefore:

- applies only to a MACHINE whose `MACHINE_FEATURES` contains
  **`alp-security`** (the N48 machines set it; no N44 machine does). Without
  the feature the package's recipes and our bbappends are masked, so the
  package layer can sit in `bblayers.conf` of a shared build directory
  without touching an N44 build;
- refuses (`bb.fatal`) `alp-security` on a MACHINE whose `ALP_SDK_SOC` is an
  N44 variant, unless `ALP_RZ_SEC_ALLOW_N44_STANDIN = "1"`, which exists to
  build-check the recipes before N48 hardware does and must never be set for
  a product.

What an N44 does with a signed/OP-TEE image is not tested (no hardware); the
design intent is that it must not boot rather than boot unprotected.

## Enabling it

1. Obtain the package, extract `meta-rz-features_security.tar.gz` next to
   your other `meta-rz-features/*` layers and add `meta-rz-security` to
   `bblayers.conf`. The glue checks the package is exactly v4.2.0.1
   (SHA-256 of three of its files) and stops the parse otherwise;
   `ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG = "1"` waives that, at your own risk.
2. Build an N48 MACHINE (or, for a build check only, an N44 one with
   `MACHINE_FEATURES:append = " alp-security"` and
   `ALP_RZ_SEC_ALLOW_N44_STANDIN = "1"`).
3. Switch on what you want, in `local.conf`. All default to `0`:

   ```
   ENABLE_SPD_OPTEE   = "1"   # OP-TEE as BL32, tee-supplicant in the image
   ENABLE_RZ_SCE      = "1"   # Secure IP driver in OP-TEE (needs the library from the package)
   TRUSTED_BOARD_BOOT = "1"   # sign + certificate-chain BL2/BL31/BL32/BL33
   ```
4. For `TRUSTED_BOARD_BOOT = "1"` provide **your own** keys (never committed,
   never in a layer):

   ```
   ALP_SEC_KEY_DIR       = "/path/outside/any/repo/keys"
   ALP_SEC_TA_SIGN_KEY   = "/path/outside/any/repo/ta_sign.pem"   # RSA, as OP-TEE expects
   ALP_SEC_IMG_AUTH_MODE = "encsign"    # or "sign": no image encryption
   ```
   `ALP_SEC_KEY_DIR` must hold `root_of_trust_key.pem`, `bl2_key.pem`,
   `bl31_key.pem`, `bl33_key.pem`, `bl32_key.pem` (with OP-TEE) and, for
   `encsign`, `cmn_key_idx0.txt`. Unset, the signing task fails. OP-TEE's TA
   signing key defaults to a *public* development key; a secure-boot build
   refuses to use it and any other build warns. Keys come from your own key
   ceremony; the key files in the Renesas package are samples.

`alp-image-*` pick up `optee-client` when all of the above gates hold;
`alp-image-edge` also gets the package's test/example/TA-sample tools (the
sample can rewrite boot flash: keep it out of production images).

## Boot media and layout

The signed images the package produces keep the layout we already use: BL2
at sector 1 / FIP at sector `0x300` of the eMMC boot partition (xSPI: BL2
`0x0`, FIP `0x60000`). Our patches to TF-A (CM33 start from xSPI, DDR) and
U-Boot (persistent environment on eMMC, signed FIT boot, Mender A/B) apply
unchanged on top of the package's flags; the package's own TF-A source
patches target the V2H and are not used (the V2N BSP TF-A already carries
them), and its U-Boot environment-offset patch targets a defconfig we do not
build. **The CM33 image that BL2 starts from xSPI is not part of the
package's certificate chain**: treat it as unauthenticated until that is
verified on an N48.

## Provisioning (what you do on a device, not what this layer does)

Building signed images writes nothing to the chip. Turning secure boot on is
a factory step with Renesas' Security Configurator and the Security Key
Management Tool; in order:

1. Generate your keys offline (root of trust, image signing, image
   encryption, key-update key, user factory programming key) and have the
   Renesas Key Wrap Service wrap the programming key.
2. Boot the Security Configurator over SCIF; the first boot writes the
   device's **Security ID** (**irreversible**; derives the hardware unique
   key), then power-cycle.
3. Load the root-of-trust public-key hash, image decryption key and key
   update key, and review the table it prints.
4. Write. **Irreversible, for each of: secure boot enable; the hash of the
   root-of-trust key (lose the key or program the wrong hash and the part
   never boots signed firmware again); image decryption keys; JTAG
   authentication mode or JTAG disable; anti-rollback enable (the version
   counter only moves up); one-time-read and write-protection bits.**
5. Flash the signed images, boot, check. Keep a development board that is
   never provisioned.

Nothing in this layer runs any of those steps, and there is deliberately no
variable that would. Do 4 only on boards you can afford to lose.

## Not covered

Anything on silicon; encrypted-image and anti-rollback flows beyond the
signing hooks; key management for production. Release note restriction from
the package: disable the `watchdog` service before running OP-TEE
applications (its timeout is shorter than the OP-TEE calls).
