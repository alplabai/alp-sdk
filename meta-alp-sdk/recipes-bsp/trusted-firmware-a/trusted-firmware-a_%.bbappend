# meta-alp-sdk: inject the E1M-V2N SoM's custom LPDDR4X DDR init into
# BL2 for the Renesas TF-A (trusted-firmware-a) build.
#
# The ONLY ALP-custom bootloader content is the BL2 DDR parameters: the
# `alp` LPDDR4X config (L4X.R2W32X16D16S32.ADEE) generated from the SoM's
# memory layout via Renesas gen_tool v3.0.2 / AN R01AN7349. Everything else
# (BL31 + U-Boot/FIP, PLAT=v2n BOARD=evk_1, the PMIC-removal + ether-setting
# U-Boot patches) is STOCK Renesas BSP.
#
# This bbappend drops the alp DDR param file over the stock v2n one before
# compile. Validated artifact: BL2 brings DDR up to 7.9 GiB and boots
# BL31+U-Boot on the e1m-x + v2n-m1 board (TFA SRCREV 4092464, the rev this
# recipe already pins for rzv2n-family).
#
# PUBLIC / PRIVATE SPLIT: this bbappend (the recipe logic) is PUBLIC -- it is
# not sensitive. The DDR param SOURCE (ddr_param_def_lpddr4-alp.c) is
# Renesas-gen_tool-derived, SoM-hardware-specific config and is NOT in this
# public repo. It is supplied at build time by the private
# `alp-sdk-internal/meta-alp-sdk` overlay layer (placed at higher BBLAYERS
# priority), which carries the file in this recipe's ${PN} dir so the SRC_URI
# below resolves to it via FILESEXTRAPATHS. A public-only build (without the
# overlay) fails fast in the bbfatal below. The prebuilt bl2/fip likewise live
# in alp-sdk-internal (production-flashed onto the SoM xSPI by ALP; the
# customer's normal flow never rebuilds the bootloader).
#
# STATUS: UNVALIDATED through bitbake (the equivalence-to-manual pass is pending).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:rzv2n-family = " file://ddr_param_def_lpddr4-alp.c"

# Reproducible / traceable BL2+BL31 version string.  TF-A's Makefile
# derives BUILD_STRING from `git describe --always --dirty --tags` when
# it is unset, which on our build is ALWAYS "-dirty": do_configure
# overwrites the tracked ddr_param_def_lpddr4.c (above), so the TF-A
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

do_configure:append:rzv2n-family() {
    if [ -f "${WORKDIR}/ddr_param_def_lpddr4-alp.c" ]; then
        install -m 0644 "${WORKDIR}/ddr_param_def_lpddr4-alp.c" \
            "${S}/${ALP_TFA_DDR_DST}"
        bbnote "meta-alp-sdk: installed alp LPDDR4X DDR params into BL2"
    else
        bbfatal "meta-alp-sdk: ddr_param_def_lpddr4-alp.c missing -- supply it via the private alp-sdk-internal/meta-alp-sdk overlay (recipes-bsp/trusted-firmware-a/trusted-firmware-a/)"
    fi
}
