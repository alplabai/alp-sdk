# meta-alp-sdk: inject the E1M-V2N SoM's custom LPDDR4X DDR init into
# BL2 for the Renesas TF-A (trusted-firmware-a) build.
#
# The ONLY ALP-custom bootloader content is the BL2 DDR parameters: the
# `alp` LPDDR4X config (L4X.R2W32X16D16S32.ADEE) generated from the SoM's
# memory layout via Renesas gen_tool v3.0.2 / AN R01AN7349 -- the
# rzv2n-family default, currently applied to e1m-v2n101-a55/e1m-v2n102-a55/
# e1m-v2m101-a55/e1m-v2m102-a55. The E1M-V2N103/E1M-V2M103 4 GB / 16 GB
# memory-tier SKUs need the 2-rank/8 Gb-per-channel sibling config
# (L4X.R2W32X16D8S32.ADEE) instead -- bench-proven 2026-09-24 -- SKU-scoped
# below via ALP_TFA_DDR_SRC, keyed on the exact x103 MACHINE names so it can
# never be picked up by e1m-v2n101-a55/e1m-v2m101-a55.
#
# OPEN (maintainer decision 2026-09-24): e1m-v2n101-a55/e1m-v2m101-a55's OWN
# catalogue entry (metadata/e1m_modules/E1M-V2N101.yaml, E1M-V2M101.yaml:
# dram_mbit: 32768 = 4 GB) states the same 4 GB this D8S32 config targets,
# yet those MACHINEs still ship the D16S32/8 GB default above. The
# maintainer's call: firmware STAYS D16S32 for V2N101/V2M101 -- this is
# DECIDED, not a placeholder -- but which of the two (catalogue or firmware)
# reflects the real production DRAM part/tier remains undecided and is a
# PRODUCTION BLOCKER (see the OPEN comment in e1m-v2n101-a55.conf /
# e1m-v2m101-a55.conf). Do not read "8 GB" anywhere in this file as a
# settled fact about V2N101/V2M101's true DRAM size, only as the config
# those MACHINEs build with today.
#
# DECIDED + APPLIED (maintainer, 2026-09-24): meta-rz-drpai's DDR
# MC-arbitration register patch (0000-ddr_param_def_lpddr4-rzv2n_1.patch:
# param_setup_mc 0x0134/0x0135/0x0178/0x017f/0x0181/0x02cd/0x02cf/0x02d0) is
# folded into BOTH ddr_param_def_lpddr4-alp*.c files below (alp-sdk-internal
# @9f88a4d) -- our do_compile:prepend replaces the WHOLE file after do_patch
# has applied that vendor patch, so on any DRP-AI-enabled build our file was
# silently discarding those arbitration tweaks before this. The 8 values are
# density-independent (identical across D16S32/D8S32, confirmed) and don't
# collide with the D8 range regs (0x012f-0x0133) or tRFC rows (0x0046-0x0060).
# param_phyinit_2d_dat1[15] is untouched by the drpai patch (confirmed: that
# patch has no hunk on param_phyinit_2d_dat1 at all) -- left at the ALP value
# (0x0100) per the maintainer's call. Rebuilt from the committed SHA on
# alplab-gw (e1m-v2m103-a55 BL2, e1m-v2n101-a55 TF-A) and confirmed via the
# built bl2.bin that all 8 entries carry the folded-in values. See
# alp-sdk-internal docs/bootloader-equivalence-verdict.md
# (DRP-AI-ARBITRATION-FOLD-IN entry) for the full table. NOT YET
# silicon-verified -- needs a cold-boot DDR training pass, memtester, and a
# DRP-AI inference run on the bench.
#
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
# build (without the overlay) fails at do_fetch -- SRC_URI can't resolve a
# file:// entry absent from every layer's FILESEXTRAPATHS -- not in the
# do_compile:prepend bbfatal below, which only fires in the narrower case
# where the file exists but do_fetch/do_unpack somehow didn't place it in
# ${WORKDIR}. The prebuilt bl2/fip likewise live in alp-sdk-internal
# (production-flashed onto the SoM xSPI by ALP; the customer's normal flow
# never rebuilds the bootloader).
#
# STATUS: the DDR-param file swap is now confirmed through bitbake (2026-09-24,
# alplab-gw): the installed source is byte-identical to the intended
# ddr_param_def_lpddr4-alp*.c and BL2/FIP/u-boot compile clean for
# e1m-v2n103-a55/e1m-v2m103-a55 (D8S32) and e1m-v2m101-a55 (byte-identical
# to the intended ALP D16S32 file). It was NOT actually running before this --
# do_configure is [noexec] in meta-arm's trusted-firmware-a.inc, so the old
# do_configure:append hook below was silently skipped by every prior bitbake
# build; see do_compile:prepend below for the fix. Bench-boot of a
# bitbake-produced FIP is still pending -- the 7.9 GiB DDR bring-up + boot
# proof above is from the manual FIP flow, which never went through this task.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

# DDR param source file: 8 GB/D16S32 by default, 4 GB/D8S32 for the two x103
# memory-tier MACHINEs. Keyed on the exact MACHINE name (not on a shared
# override like e1m-v2m101-a55/e1m-v2n101-a55) so a plain read of THIS file
# can't pull the wrong config into any other SKU.
#
# OVERRIDE-PRECEDENCE HAZARD (read before adding another ALP_TFA_DDR_SRC
# line): this is a plain (non-:append) override assignment, and bitbake
# resolves multiple matching plain overrides by OVERRIDES rank -- the
# rightmost-listed override wins. e1m-v2n103-a55.conf sets
# `MACHINEOVERRIDES =. "e1m-v2n103-a55:e1m-v2n103:e1m-v2n101-a55:e1m-v2n101:"`
# (e1m-v2m103-a55.conf similarly also lists e1m-v2m101-a55) -- so
# e1m-v2n101-a55/e1m-v2m101-a55 sit to the RIGHT of the x103 names in an
# x103 MACHINE's own OVERRIDES and would outrank them. A future
# `ALP_TFA_DDR_SRC:e1m-v2n101-a55 = "..."` line (added for some unrelated
# V2N101 reason, with no memory of this file) would then SILENTLY win over
# the x103-keyed lines above on x103 MACHINEs too, reverting them to the
# wrong DDR param with no error. Don't trust override discipline to catch
# that -- do_compile:prepend below asserts the resolved value instead.
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
# dependents without running it. See the STATUS block near the top of this
# file for the history: this used to be a do_configure:append and was
# consequently a silent no-op for every bitbake build to date. do_compile is
# not noexec (it runs the actual `make`), so prepending here runs before BL2
# compiles, same ordering intent as the old do_configure hook.
do_compile:prepend:rzv2n-family() {
    if [ -f "${WORKDIR}/${ALP_TFA_DDR_SRC}" ]; then
        install -m 0644 "${WORKDIR}/${ALP_TFA_DDR_SRC}" \
            "${S}/${ALP_TFA_DDR_DST}"
        bbnote "meta-alp-sdk: installed alp LPDDR4X DDR params (${ALP_TFA_DDR_SRC}) into BL2"
    else
        bbfatal "meta-alp-sdk: ${ALP_TFA_DDR_SRC} missing -- supply it via the private alp-sdk-internal/meta-alp-sdk overlay (recipes-bsp/trusted-firmware-a/trusted-firmware-a/)"
    fi

    # Assert the override-precedence hazard documented above ALP_TFA_DDR_SRC
    # never actually reverted an x103 MACHINE to the 8 GB file -- catches any
    # future ALP_TFA_DDR_SRC:e1m-v2n101-a55/:e1m-v2m101-a55 override collision
    # at build time instead of shipping the wrong DDR size silently.
    case "${MACHINE}" in
        e1m-v2n103-a55|e1m-v2m103-a55)
            if [ "${ALP_TFA_DDR_SRC}" != "ddr_param_def_lpddr4-alp-d8s32.c" ]; then
                bbfatal "meta-alp-sdk: ALP_TFA_DDR_SRC resolved to '${ALP_TFA_DDR_SRC}' for MACHINE=${MACHINE} -- expected ddr_param_def_lpddr4-alp-d8s32.c. An override-precedence collision (see the comment above ALP_TFA_DDR_SRC) reverted this x103 MACHINE to the wrong DDR param."
            fi
            ;;
    esac
}
