# SPDX-License-Identifier: LicenseRef-DEEPX-source-visible
#
# Pins the DEEPX runtime + kernel driver.  Vendor-licensed
# source-visible per docs/vendor-partnerships.md §C.31; redistribution
# requires a per-customer agreement with DEEPX.  This recipe pulls
# from the customer's own DEEPX SDK clone -- the upstream public
# mirror does NOT include the runtime, only the firmware.

SUMMARY = "DEEPX DX-M1 host runtime + Linux kernel driver"
DESCRIPTION = "Userspace dx_rt + kernel module that talks to the \
               DEEPX DX-M1 NPU over PCIe.  Pairs with the v0.5 \
               alp-sdk inference dispatcher when AUTO selects \
               ALP_INFERENCE_BACKEND_DEEPX_DX on V2N-M1."
HOMEPAGE = "https://github.com/DEEPX-AI"

LICENSE = "LicenseRef-DEEPX-source-visible"
LIC_FILES_CHKSUM = "file://LICENSE;md5=00000000000000000000000000000000"

# Customer-private source tree.  Upstream:
#   git://github.com/DEEPX-AI/dx_rt.git
# but it requires authenticated access; the standard pattern is
# to mirror locally + drop a tar into DL_DIR.
SRC_URI = "${SP_MIRROR}/dx_rt-2.4.0.tar.gz"
SRC_URI[sha256sum] = "00000000000000000000000000000000000000000000000000000000000000000"

PV = "2.4.0"

S = "${WORKDIR}/dx_rt-${PV}"

inherit module cmake

EXTRA_OECMAKE = "-DDX_RT_TARGET=DXM1 -DDX_RT_BUILD_DRIVER=ON"

# Builds dx_rt userspace + the in-tree kernel module
# (`kernel-module-dx-rt-npu`).
KERNEL_MODULE_AUTOLOAD = "dx_rt_npu"

FILES:${PN}     += "${libdir}/libdx_rt.so.*  ${bindir}/dxrt_cli"
FILES:${PN}-dev += "${libdir}/libdx_rt.so    ${includedir}/dxrt/*.h"

# Pinned in docs/test-plan.md.  Update alongside the v0.8 V2M HiL
# bring-up.
