# meta-alp-sdk: inject the E1M-V2N SoM's custom LPDDR4X DDR init into
# BL2 for the Renesas TF-A (trusted-firmware-a) build.
#
# The ONLY ALP-custom bootloader content is the BL2 DDR parameters: the
# `alp` LPDDR4X config (L4X.R2W32X16D16S32.ADEE, 8 GB) generated from the SoM's
# memory layout via Renesas gen_tool v3.0.2 / AN R01AN7349. The E1M-V2N103 and
# E1M-V2M103 4 GB / 16 GB memory-tier SKUs need the 2-rank/8 Gb-per-channel
# sibling config (L4X.R2W32X16D8S32.ADEE) instead -- SKU-scoped below via
# ALP_TFA_DDR_SRC, keyed on the exact x103 MACHINE names so it can never be
# picked up by the v2n101/v2m101 8 GB SKUs those MACHINEs also override.
# Everything else (BL31 + U-Boot/FIP, PLAT=v2n BOARD=evk_1, the PMIC-removal +
# ether-setting U-Boot patches) is STOCK Renesas BSP.
#
# This bbappend drops the alp DDR param file over the stock v2n one before
# compile. Validated artifact: BL2 brings DDR up to 7.9 GiB and boots
# BL31+U-Boot on the e1m-x + v2n-m1 board (TFA SRCREV 4092464, the rev this
# recipe already pins for rzv2n-family).
#
# PUBLIC / PRIVATE SPLIT: this bbappend (the recipe logic) is PUBLIC -- it is
# not sensitive. The DDR param SOURCES (ddr_param_def_lpddr4-alp.c and
# ddr_param_def_lpddr4-alp-d8s32.c) are Renesas-gen_tool-derived,
# SoM-hardware-specific config and are NOT in this public repo. They are
# supplied at build time by the private `alp-sdk-internal/meta-alp-sdk`
# overlay (rsync'd onto this public layer tree -- see that repo's
# conf/bblayers-overlay.md), which carries both files in this recipe's ${PN}
# dir so the SRC_URI below resolves them via FILESEXTRAPATHS. A public-only
# build (without the overlay) fails fast in the bbfatal below. The prebuilt
# bl2/fip likewise live in alp-sdk-internal (production-flashed onto the SoM
# xSPI by ALP; the customer's normal flow never rebuilds the bootloader).
#
# STATUS: UNVALIDATED through bitbake (the equivalence-to-manual pass is pending).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# DDR param source file: 8 GB/D16S32 by default, 4 GB/D8S32 for the two x103
# memory-tier MACHINEs. Keyed on the exact MACHINE name (not on a shared
# override like e1m-v2m101-a55/e1m-v2n101-a55) so it can't be pulled in by any
# other SKU those two MACHINEs' MACHINEOVERRIDES chains also list.
ALP_TFA_DDR_SRC ?= "ddr_param_def_lpddr4-alp.c"
ALP_TFA_DDR_SRC:e1m-v2n103-a55 = "ddr_param_def_lpddr4-alp-d8s32.c"
ALP_TFA_DDR_SRC:e1m-v2m103-a55 = "ddr_param_def_lpddr4-alp-d8s32.c"

SRC_URI:append:rzv2n-family = " file://${ALP_TFA_DDR_SRC}"

# Reproducible / traceable BL2+BL31 version string.  TF-A's Makefile
# derives BUILD_STRING from `git describe --always --dirty --tags` when
# it is unset, which on our build is ALWAYS "-dirty": do_compile:prepend
# overwrites the tracked ddr_param_def_lpddr4.c (below), so the TF-A
# source tree is intentionally modified at build time.  The result was
# a permanently "-dirty" boot banner ("v2.10.5(release):4092464-dirty")
# that also leaked the upstream short SHA.  Pin BUILD_STRING to a clean,
# deterministic ALP id instead.  The per-SKU release pipeline overrides
# ALP_TFA_BUILD_STRING with the signed-bundle version for full
# traceability; the default just guarantees no "-dirty" / no SHA leak.
ALP_TFA_BUILD_STRING ?= "alp"
# Single-quote so a release-pipeline override reaches make as one arg
# (oe_runmake word-splits EXTRA_OEMAKE on whitespace).  The release
# version is schema-locked to a whitespace-free token (som-X.Y.Z), so
# the default is safe regardless; the quote is defensive.
EXTRA_OEMAKE:append:rzv2n-family = " BUILD_STRING='${ALP_TFA_BUILD_STRING}'"

# Path of the stock DDR param file inside the TF-A source tree.
ALP_TFA_DDR_DST ?= "plat/renesas/rz/soc/v2n/drivers/ddr/ddr_param_def_lpddr4.c"

# Hooked onto do_compile, NOT do_configure: meta-arm's trusted-firmware-a.inc
# sets `do_configure[noexec] = "1"` (TF-A's build is plain make, no separate
# configure step), so a `do_configure:append` function body is added but NEVER
# EXECUTED -- bitbake skips a noexec task's body entirely, satisfying its
# dependents without running it. That is exactly what "STATUS: UNVALIDATED
# through bitbake" above was flagging: this file swap has only ever been
# proven through the manual FIP build flow. do_compile is not noexec (it runs
# the actual `make`), so prepending here runs before BL2 compiles, same
# ordering intent as the old do_configure hook.
do_compile:prepend:rzv2n-family() {
    if [ -f "${WORKDIR}/${ALP_TFA_DDR_SRC}" ]; then
        install -m 0644 "${WORKDIR}/${ALP_TFA_DDR_SRC}" \
            "${S}/${ALP_TFA_DDR_DST}"
        bbnote "meta-alp-sdk: installed alp LPDDR4X DDR params (${ALP_TFA_DDR_SRC}) into BL2"
    else
        bbfatal "meta-alp-sdk: ${ALP_TFA_DDR_SRC} missing -- supply it via the private alp-sdk-internal/meta-alp-sdk overlay (recipes-bsp/trusted-firmware-a/trusted-firmware-a/)"
    fi
}
