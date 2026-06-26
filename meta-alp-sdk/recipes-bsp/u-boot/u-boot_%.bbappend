# meta-alp-sdk: bring up the on-module DEEPX DX-M1 in U-Boot, before the
# A55 kernel's PCIe root complex probes.
#
# WHY: the V2N-M1 SoM wires the DX-M1 behind two passive PI3DBS12212A PCIe
# muxes (PD on Renesas P80, SEL on P95) and an active-low reset M1_RESET
# (PA6). The A55 Linux PCIe RC (rzg3s-pcie-host) trains the link in its
# builtin probe very early (~1.7s) and the driver has no gpio/reset hook,
# so Linux gpio-hogs apply too late -- the RC trains into a powered-down
# mux and fails with -ETIMEDOUT. Boot is A55-only here, so the only layer
# that runs before the RC is U-Boot.
#
# WHAT: board_late_init() (board/renesas/rzv2n-dev/rzv2n-dev.c) reads the
# on-module hardware-info manifest from the RIIC0 24C128 EEPROM (0x50),
# validates magic + schema_version + CRC32 (EEPROM-MANIFEST-SPEC.md), and
# ONLY when the manifest reports family "v2n-m1" sets, before bootcmd:
#     P80 = low   -> enable the PI3DBS12212A muxes (PD active-low)
#     P95 = low   -> route PCIe to the DEEPX path (path_0)
#     PA6 = high  -> release M1_RESET (active-low)
# So the same U-Boot is safe on a non-DEEPX V2N SoM (no manifest match ->
# DEEPX path left untouched). RIIC0 (P30/P31) pinmux is added to s_init and
# the i2c0 node enabled in rzv2n-dev.dts. (Pair with the kernel dtb's pcie
# num-lanes=2.) DEEPX rails are always-on (current SoM rev's standalone
# buck), so no rail sequencing is needed.
#
# Targets the renesas-u-boot-cip SRCREV this BSP pins for rzv2n-family
# (2024.07, bcf29d98); applies on top of meta-renesas's rzv2n-dev PMIC-I2C
# removal patch.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:rzv2n-family = " \
    file://0001-rzv2n-dev-EEPROM-gated-DEEPX-DX-M1-PCIe-bring-up.patch \
    file://0002-rzv2n-dev-ALP-E1M-production-boot.patch \
    file://no-dirty-version.cfg \
"

# no-dirty-version.cfg: the vendor defconfig's CONFIG_LOCALVERSION_AUTO
# appends `git describe --dirty` -- always "-dirty" here because the
# patches above are git-applied into the tree -- leaking a "-dirty"
# flag + the upstream SHA into the boot banner. The fragment disables
# the auto version and pins an ALP localversion. Merged into the
# resolved defconfig by u-boot-configure.inc (find_cfgs() + merge_config.sh
# pick up any *.cfg in SRC_URI) -- the same path prod-boot.cfg uses.

# 0002 (production boot): two build-gated ALP additions to the same
# rzv2n-dev board files.
#   CONFIG_ALP_E1M_EMMC_1V8 (default y in the patched defconfig):
#     drive eMMC_V_SEL (PA0) high in board_init, BEFORE the first MMC
#     access (initr_env loads the environment from eMMC right after
#     board_init in init_sequence_r) -- the E1M SoM's eMMC/SD0 IO rail
#     powers up at 3.3 V and eMMC has no live VccQ-switch handshake,
#     so the whole chain must run the card at 1.8 V from the start.
#     Pairs with the Linux-side PA0 gpio-hog (e1m-v2n-som.dtsi).
#   CONFIG_ALP_PROD_BOOT (default n, DEV-target only): production
#     kernel cmdline (console= pinned + quiet, earlycon dropped). The
#     dev cmdline keeps earlycon but gains console=ttySC0,115200,
#     which stops the kernel replaying the early log across the
#     console handover. The cmdline is rebuilt at CONFIG_BOOTCOMMAND
#     (patch-safe vs the build-varying env block); the future per-SKU
#     fdtfile derivation must also happen there, AFTER the leading
#     'env default -a' wipe -- see the comment in the patch.
# VALIDATION: bitbake-built dev + prod with config asserts; the FIP
# (BL2+BL31+u-boot, manual flow) was built 2026-06-12 with both ALP
# patches and the u-boot binary content-verified (alp_root bootcmd +
# pinned console). On-silicon FIP flash + boot is PENDING maintainer
# authorization (a persistent bootloader write to the shared bench
# board). The manual FIP flow (build_custom_fip_v630_deepx.sh, WSL)
# was updated the same day to apply 0002 alongside the DEEPX patch and
# to verify it (strings u-boot | grep 'setenv alp_root').

# Production boot lockdown (BOOTDELAY=0 + keyed autoboot + the prod
# cmdline above): opt-in for release-bundle builds only. An
# un-overridden prod build has an EMPTY stop string (= no stop
# sequence at all); the internal release pipeline injects the real
# per-product stop string. See prod-boot.cfg for the lock mechanism
# and the deferred saved-env hole.
ALP_PROD_BOOT ?= "0"
SRC_URI:append:rzv2n-family = "${@' file://prod-boot.cfg' if bb.utils.to_boolean(d.getVar('ALP_PROD_BOOT')) else ''}"

# Build U-Boot with the rzv2n-dev config, not the machine's stock rzv2n-evk.
# The DEEPX bring-up patched above lives in board/renesas/rzv2n-dev/rzv2n-dev.c,
# which is ONLY compiled under CONFIG_TARGET_RZV2N_DEV -- so the dev config is
# mandatory, not cosmetic. It also reproduces the silicon-validated manual
# bootloader (DEV target, env-in-MMC, Renesas clock driver off). The longer-term
# rzv2n-dev-vs-rzv2n-evk production-config decision (clock driver, env storage)
# is deliberately deferred -- see the alp-sdk-internal som-productization docs.
UBOOT_CONFIG[rzv2n-evk] = "rzv2n-dev_defconfig"

# Demote patch-fuzz from fatal-error to warning for THIS recipe only.
# u-boot here is renesas-u-boot-cip pinned via +git (a moving base), and the
# vendor feature-layer patches (meta-rz-drpai add-ether, meta-rz-opencva
# OpenCVA+Codec) apply on top of it. Those vendor hunks land with a small
# context offset -- they apply correctly ("Hunk succeeded"), but Yocto's fatal
# patch-fuzz QA fails do_patch. The offset is structural, not a defect in our
# patches: the OpenCVA hunk in include/configs/rzv2n-evk.h fuzzes by the same
# 5 lines even though NO ALP patch touches evk.h -- that is pure +git base
# drift. (The ALP 0002 patch additionally shifts rzv2n-dev.h, compounding it.)
# Refreshing vendor patches each BSP bump is a treadmill and we do not own
# them, so demote the gate here rather than mask fuzz globally.
WARN_QA:append = " patch-fuzz"
ERROR_QA:remove = "patch-fuzz"
