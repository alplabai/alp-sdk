# RZ/V2N security: secure boot and OP-TEE (N48 only)

Opt-in glue between `meta-alp-sdk` and the Renesas **RZ MPU Security Package
for RZ/V2N AI SDK** (v4.2.0.1, for AI SDK v6.30, our BSP). The package is
licence-gated and comes from Renesas under your own account (search "RZ MPU
Security Package (Linux OS)" on renesas.com); Alp Lab does not redistribute it
and nothing from it is in this repository.

**Status.** On `e1m-v2n104-a55` `bitbake trusted-firmware-a optee-os
firmware-signing optee-ta-sample optee-example` builds with throwaway keys, and
`bitbake -p` passes for all six N48 machines. The signed `firmware-pack`
images, an `alp-image-*` image and the boot flow have not been built at this
head, and **nothing has run on silicon.**

## What it gives you

- Secure boot: the ROM verifies BL2, BL2 verifies BL31, BL32 and BL33 against
  certificates signed with your keys (optionally encrypted images).
- A trusted execution environment: OP-TEE OS as BL32 plus the OP-TEE client in
  Linux.
- Access to the SoC's hardware crypto block through OP-TEE, with the Renesas
  library. It is not reachable through OpenSSL, PKCS#11 or the standard TEE
  crypto API; see the package documentation.

## N48 only

These features exist only on the N48 security device. The N44 behind
`E1M-V2N101..103` / `E1M-V2M101..103` is a Renesas *normal device*: the
features are disabled in silicon. The glue therefore:

- applies only to a MACHINE with the **`alp-security`** `MACHINE_FEATURE`,
  which `conf/machine/include/e1m-n48.inc` sets for the six N48 machines.
  Without it the package's recipes and our bbappends are masked, so the
  package layer can sit in `bblayers.conf` of a shared build directory
  without touching an N44 build;
- refuses (`bb.fatal`) `alp-security` on a MACHINE without the `e1m-n48`
  override, with no way around it;
- refuses (`bb.fatal`, `classes/alp-rz-security-guard.bbclass`, inherited by
  `DISTRO = "alp"`) a build that sets `TRUSTED_BOARD_BOOT`,
  `ENABLE_SPD_OPTEE` or `ENABLE_RZ_SCE` to `"1"` when the package layer is
  not in `bblayers.conf` or the MACHINE is not an N48. Otherwise those
  switches would be ignored and the build would quietly produce unsigned
  firmware. A distro other than `alp` does not get this check.

What an N44 does with a signed image is untested.

## Enabling it

1. Obtain the package, extract `meta-rz-features_security.tar.gz` next to your
   other `meta-rz-features/*` layers and add `meta-rz-security` to
   `bblayers.conf`. The glue checks the package is exactly v4.2.0.1 (SHA-256
   of three of its files) and stops the parse otherwise;
   `ALP_RZ_SEC_ACCEPT_UNKNOWN_PKG = "1"` waives that, at your own risk.
2. Build an N48 MACHINE (`e1m-v2n104..106-a55`, `e1m-v2m104..106-a55`).
3. Switch on what you want in `local.conf`; all default to `0`:

   ```
   ENABLE_SPD_OPTEE   = "1"   # OP-TEE as BL32, OP-TEE client in the image
   ENABLE_RZ_SCE      = "1"   # hardware crypto driver in OP-TEE
   TRUSTED_BOARD_BOOT = "1"   # sign and certificate-chain the firmware
   ```
4. For `TRUSTED_BOARD_BOOT = "1"` give it **your own** keys, outside every
   repository and layer; the package's sample keys are not for products:

   ```
   ALP_SEC_KEY_DIR       = "/path/outside/any/repo/keys"
   ALP_SEC_TA_SIGN_KEY   = "/path/outside/any/repo/ta_sign.pem"   # RSA
   ALP_SEC_IMG_AUTH_MODE = "encsign"    # or "sign": no image encryption
   ```
   `ALP_SEC_KEY_DIR` holds the files the package's `firmware-signing` recipe
   documents (root-of-trust, per-image signing keys and, for `encsign`, an
   image encryption key). Unset, the signing task fails. OP-TEE signs Trusted
   Applications with a *public* development key by default: a secure-boot
   build refuses it and any other build warns. The TA key is never copied into
   the OP-TEE dev kit that other recipes build against (the glue strips it); the
   TA recipes get it from `ALP_SEC_TA_SIGN_KEY` directly.

`alp-image-*` get `optee-client` when all gates hold. The package's test,
example and sample tools (the sample can rewrite boot flash) go only into
`alp-image-edge` and only with `ALP_SEC_DEV_TOOLS = "1"` (default `0`).

## Outputs and boot media

With `TRUSTED_BOARD_BOOT = "1"`, `firmware-pack` deploys the signed images next
to the normal ones: `bl2-tbb_bp_emmc-${MACHINE}.bin` (also `_spi`, `_esd`),
`fip-tbb-${MACHINE}.bin` and `Flash_Writer_SCIF_TBB_RZV2N_DEV_LPDDR4X.mot`. The
layout is unchanged (eMMC boot0: BL2 and FIP at the offsets we already use).
`provision_som.py` still bundles the **normal** (unsigned) FIP: flash the
`-tbb` files by hand. After secure boot is enabled in the chip, **unsigned BL2,
FIP and flash writer will not boot.**

Our TF-A and U-Boot patches apply unchanged on top of the package's flags. The
package's own TF-A and U-Boot patches are not used. **The CM33 image that BL2
starts from xSPI is not part of the package's certificate chain**: treat it as
unauthenticated until verified on an N48.

## Provisioning

Building signed images writes nothing to the chip. Enabling secure boot is a
separate factory step with Renesas' tools that **writes one-time-programmable
memory irreversibly**: a wrong key hash, a lost root-of-trust key, or a debug
lock set too early can leave a part that never boots your firmware again.
Follow the Renesas security manual that ships with the package, on development
boards you can afford to lose. This layer runs none of it, and there is
deliberately no variable that would.
