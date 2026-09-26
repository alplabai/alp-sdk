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

# GATED behind an explicit opt-in: DEEPX's dx-driver/dx-rt recipes
# declare LICENSE = "Proprietary", and the dx_rt / dx_rt_npu_linux_driver
# source they fetch carries DEEPX's customer-only licence terms
# ("provided exclusively to customers who are supplied with DEEPX
# NPU" -- see docs/vendor-partnerships.md's DEEPX section; note the
# driver source files also carry SPDX GPL-2.0 headers in places, an
# unresolved ambiguity in DEEPX's own upstream, not ours to resolve
# here).  meta-deepx-m1 itself ships no LICENSE file. A build must
# both carry the layer AND opt in here. Set ALP_ENABLE_DEEPX_DXM1 =
# "1" in local.conf once meta-deepx-m1 is in bblayers.conf.
ALP_ENABLE_DEEPX_DXM1 ?= "0"

# `dx-driver` builds the DX-M1 PCIe kernel module(s) (`dx_dma.ko` +
# `dxrt_driver.ko`).  KERNEL_MODULE_AUTOLOAD below is defensive, not
# strictly required: `dx_dma` carries a PCI MODULE_DEVICE_TABLE, so udev
# already autoloads it via the standard modalias path, and dx-driver's
# own `dx_dma.conf` modprobe.d snippet declares
# `softdep dx_dma pre: dxrt_driver`, which pulls `dxrt_driver` in ahead
# of it -- but an explicit KERNEL_MODULE_AUTOLOAD is the documented,
# layer-owned way to guarantee both load at boot regardless of udev/
# initramfs timing on a given image.  `dx-rt` is the userspace DXRT
# runtime; there is no separate `dx-rt-cli` package at this pin --
# bench-verified against a real bake of meta-deepx-m1@8d09b25f's
# dx-rt_3.2.0: its RPM deploy set is only `dx-rt` /
# `dx-rt-dbg`/`dx-rt-dev`/`dx-rt-src`, and `dxrt-cli` (plus `dxrtd`,
# `dxtop`, `dxbenchmark`) all ship IN the main `dx-rt` package.
# Installing a `dx-rt-cli` item here fails the image with "Nothing
# RPROVIDES dx-rt-cli" -- don't add it back without re-verifying
# against whatever PREFERRED_VERSION_dx-rt is pinned to below.
IMAGE_INSTALL:append = "${@' dx-driver dx-rt' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else ''}"
KERNEL_MODULE_AUTOLOAD:append = "${@' dx_dma dxrt_driver' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else ''}"

# SECURITY: upstream dx-driver ships /etc/udev/rules.d/99-dx-dma.rules
# with `KERNEL=="dxrt*", MODE="0666"` (world-writable device node).
# meta-alp-sdk/dynamic-layers/meta-deepx-m1/recipes-runtime/dx-driver/
# dx-driver_%.bbappend rewrites that to `MODE="0660", GROUP="video"`
# (parsed only when meta-deepx-m1 is in bblayers.conf, via
# BBFILES_DYNAMIC in conf/layer.conf) -- see that bbappend for the
# rationale and the loud-fail-on-drift guard.

# Pinned together, not independently: these versions match the DX-M1
# firmware 2.4.0 that ships on these SKUs -- the SoM-specific
# DEEPX-built firmware these SKUs run. Per DEEPX's own DX-RT release
# notes, firmware and runtime are coupled in lockstep:
#   FW 2.4.0 <-> DX-RT 3.1/3.2 + driver 1.8.0   (this pin)
#   FW 2.5.2 <-> DX-RT 3.3.x   + driver 2.4.x (layer ships dx-driver_2.4.1.bb)
#   FW 2.7.0 <-> DX-RT 3.4.x   + driver 2.5/2.6
# Bump PREFERRED_VERSION here ONLY together with a firmware bump --
# never independently, or dxrt-cli -s will report a runtime/firmware
# mismatch on-target.
#
# "3.2.0" deliberately selects the dx-rt_3.2.0.bb recipe, not the
# dx-rt_3.2.0-1.bb USE_SERVICE variant: BitBake splits a PREFERRED_VERSION
# containing "-" into PV-PR, so a value of "3.2.0-1" would try to match
# PV "3.2.0" at PR "1" -- it would NOT select the recipe literally named
# dx-rt_3.2.0-1.bb.  Leave the trailing "-1" off on purpose.
PREFERRED_VERSION_dx-driver = "1.8.0"
PREFERRED_VERSION_dx-rt = "3.2.0"
