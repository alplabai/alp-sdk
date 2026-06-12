# SPDX-License-Identifier: LicenseRef-DEEPX-source-visible
#
# Pins the DEEPX runtime + kernel driver.  Vendor-licensed
# source-visible per docs/vendor-partnerships.md §C.31; redistribution
# requires a per-customer agreement with DEEPX.  This recipe pulls
# from the customer's own DEEPX SDK clone -- the upstream public
# mirror does NOT include the runtime, only the firmware.

SUMMARY = "DEEPX DX-M1 host runtime + Linux kernel driver"
DESCRIPTION = "Userspace dx_rt + kernel module that talks to the \
               DEEPX DX-M1 NPU over PCIe.  Pairs with the alp-sdk \
               inference dispatcher when AUTO selects the \
               DEEPX_DXM1 backend on V2N-M1 (V2M)."
HOMEPAGE = "https://github.com/DEEPX-AI"

LICENSE = "LicenseRef-DEEPX-source-visible"
# NOTE: no real LICENSE file + checksum can be shipped here -- the DEEPX
# runtime is license-gated and is NOT redistributed in this public layer.
# The all-zero md5 below is a deliberate placeholder paired with the
# do_fetch bb.fatal stub; it never gets exercised because the recipe is
# blacklisted. Supply the real LIC_FILES_CHKSUM (against the LICENSE that
# ships with the customer's licensed DEEPX SDK) when un-gating.
LIC_FILES_CHKSUM = "file://LICENSE;md5=00000000000000000000000000000000"

# UNBUILDABLE by design: this recipe has no resolvable source, no real
# sha256, and no shippable LICENSE. DEEPX dx_rt is license-gated and is
# NOT redistributed in this public layer. Removing the skip requires
# a per-customer DEEPX agreement + a real source mirror, sha256, and
# LICENSE file. See docs/vendor-partnerships.md and the alp-sdk-internal
# repo for the licensed-vendor file placement.
# (SKIP_RECIPE, not PNBLACKLIST: scarthgap's base.bbclass only honors
# SKIP_RECIPE -- PNBLACKLIST has been inert since honister, so the old
# flag never actually skipped anything; only the do_fetch stop did.)
SKIP_RECIPE[dx-rt] ?= "DEEPX dx_rt is license-gated and not redistributed in the public meta-alp-sdk layer; provide a real source mirror + sha256 + LICENSE and clear this skip (see comments in this recipe)."
EXCLUDE_FROM_WORLD = "1"

# Customer-private source tree.  Upstream is DEEPX's github.com/DEEPX-AI
# dx_rt, which requires authenticated/licensed access; the standard
# pattern is to mirror locally + drop a tar into DL_DIR. No public URL or
# checksum is asserted here -- the do_fetch stub below hard-fails first.
SRC_URI = ""
PV = "2.4.0"

S = "${WORKDIR}/dx_rt-${PV}"

inherit module cmake

# Hard stop: refuse to fetch/build until a real licensed source + LICENSE
# + sha256 are wired in. This guards against the placeholder values ever
# being treated as buildable.
python do_fetch() {
    bb.fatal("dx-rt is license-gated and unbuildable in the public "
             "meta-alp-sdk layer: no redistributable DEEPX dx_rt source, "
             "sha256, or LICENSE is shipped. Obtain dx_rt under a DEEPX "
             "license, mirror it, set SRC_URI + SRC_URI[sha256sum] + a "
             "real LIC_FILES_CHKSUM, then clear SKIP_RECIPE[dx-rt].")
}

EXTRA_OECMAKE = "-DDX_RT_TARGET=DXM1 -DDX_RT_BUILD_DRIVER=ON"

# Builds dx_rt userspace + the in-tree kernel module
# (`kernel-module-dx-rt-npu`).
KERNEL_MODULE_AUTOLOAD = "dx_rt_npu"

FILES:${PN}     += "${libdir}/libdx_rt.so.*  ${bindir}/dxrt_cli"
FILES:${PN}-dev += "${libdir}/libdx_rt.so    ${includedir}/dxrt/*.h"

# Pinned in docs/test-plan.md.  Update alongside the v0.8 V2M HiL
# bring-up.
