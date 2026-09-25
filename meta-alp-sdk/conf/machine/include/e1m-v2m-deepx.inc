# SPDX-License-Identifier: Apache-2.0
#
# Shared DEEPX DX-M1 block for the V2M (DEEPX-populated) MACHINEs --
# e1m-v2m101-a55.conf / e1m-v2m102-a55.conf / e1m-v2m103-a55.conf all
# `require` this include instead of repeating the block three times.
#
# The DEEPX runtime is consumed from DEEPX's own OFFICIAL, publicly
# published Yocto layer -- `meta-deepx-m1`
# (github.com/DEEPX-AI/meta-deepx-m1, branch scarthgap, verified commit
# 8d09b25f20f81104c16c7de90928ff8920eb482d) -- added by the BUILDER to
# bblayers.conf (see meta-alp-sdk/README.md).  This layer no longer
# carries an in-tree `dx-rt` placeholder recipe: DEEPX now publishes
# `dx_rt` + `dx_rt_npu_linux_driver` + `meta-deepx-m1` themselves, so
# there is nothing left for meta-alp-sdk to stub or SKIP_RECIPE.

# GATED behind an explicit opt-in: meta-deepx-m1's recipes are licensed
# to DEEPX NPU customers (see docs/vendor-partnerships.md's DEEPX
# section), so a build must both carry the layer AND opt in here. Set
# ALP_ENABLE_DEEPX_DXM1 = "1" in local.conf once meta-deepx-m1 is in
# bblayers.conf.
ALP_ENABLE_DEEPX_DXM1 ?= "0"

# `dx-driver` builds the DX-M1 PCIe kernel module(s) (`dx_dma.ko` +
# `dxrt_driver.ko`) but does NOT autoload them (verified against
# upstream meta-deepx-m1) -- KERNEL_MODULE_AUTOLOAD below is required,
# not belt-and-braces.  `dx-rt` is the userspace DXRT runtime;
# `dx-rt-cli` is the `dxrt-cli` diagnostic binary used to verify the
# on-module firmware version (see docs/bring-up-v2n-m1.md).
IMAGE_INSTALL:append = "${@' dx-driver dx-rt dx-rt-cli' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else ''}"
KERNEL_MODULE_AUTOLOAD:append = "${@' dx_dma dxrt_driver' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else ''}"

# Pinned together, not independently: these versions match the DX-M1
# firmware 2.4.0 that ships on these SKUs -- the SoM-specific
# DEEPX-built firmware these SKUs run. Per DEEPX's own DX-RT release
# notes, firmware and runtime are coupled in lockstep:
#   FW 2.4.0 <-> DX-RT 3.1/3.2 + driver 1.8.0   (this pin)
#   FW 2.5.2 <-> DX-RT 3.3.x   + driver 2.4.0
#   FW 2.7.0 <-> DX-RT 3.4.x   + driver 2.5/2.6
# Bump PREFERRED_VERSION here ONLY together with a firmware bump --
# never independently, or dxrt-cli -s will report a runtime/firmware
# mismatch on-target.
PREFERRED_VERSION_dx-driver = "1.8.0"
PREFERRED_VERSION_dx-rt = "3.2.0"
