# SPDX-License-Identifier: Apache-2.0
#
# Development image for V2N / V2N-M1 / i.MX 93 edge AI bring-up.
# Built by:
#   MACHINE = "e1m-v2m101-a55" bitbake alp-image-edge
#
# = alp-image-base (the headless core: Alp SDK + NPU runtime, Mender, watchdog,
#   networkd) with EVERY opt-in feature group turned on -- the dev kitchen-sink:
#     - alp-camera:  libcamera + GStreamer
#     - alp-display: Weston/Wayland
#     - alp-ros:     ROS 2 Humble + alp-perception
#   PLUS developer conveniences:
#     - debug-tweaks: empty root password / passwordless console + SSH login
#       (bench convenience -- the production image, alp-image-prod, strips it).
#     - libdrm-tests: modetest et al. for display bring-up.
# For a hardened, customer-facing build use alp-image-prod instead.

SUMMARY = "Alp SDK edge AI development image (all features, debug-tweaks)"

require alp-image-common.inc

# Dev kitchen-sink: every opt-in feature group + passwordless debug access.
IMAGE_FEATURES += "alp-camera alp-display alp-ros debug-tweaks"

# Display/bench bring-up tooling (modetest, modeprint). Rides in rz-vlp's tools
# group too; listed explicitly so rz-bsp builds also get it. Dev-only.
IMAGE_INSTALL += " \
    libdrm-tests                   \
"

# LVGL dashboard example (Linux/DRM panel) -- dev-only bench example app.
# weston/libdrm/DEEPX/rootfs sizing now come from alp-image-common.inc;
# only the example package is edge-specific.
IMAGE_INSTALL += " alp-lvgl-dashboard"

# NOTE: alp-image-common.inc's ALP_RZ_DRPAI_INSTALL (issue #1176) already
# installs the meta-rz-drpai userspace payload UNCONDITIONALLY once the
# rz-drpai layer is present in BBFILE_COLLECTIONS -- no opt-in switch. The
# ALP_ENABLE_DRPAI-gated IMAGE_INSTALL:append below is therefore
# belt-and-suspenders for that half (bitbake dedupes repeated package
# names in IMAGE_INSTALL, so this is not a build break); what it alone
# still drives is the alp-sdk PACKAGECONFIG[drpai] compile flag
# (`PACKAGECONFIG:append:pn-alp-sdk` below), which alp-image-common.inc
# does not touch. Reconcile which mechanism owns the userspace install
# before both land for good.
#
# DRP-AI userspace (RZ/V2N on-die NPU) on the rzv2n-family machine confs:
# the machine confs own this, gated on ALP_ENABLE_DRPAI (meta-rz-drpai's
# lib-tvm ships through a core-image-% bbappend that does NOT match
# `alp-image-edge`, so the payload has to be installed explicitly, and
# doing that installation MACHINE-blind here would pull RZ-only recipes
# into every alp-image-edge, including e1m-nx9101-a55 / e1m-aen801-a32,
# which have no DRP-AI silicon -- see e1m-v2n101-a55.conf et al.). No
# unconditional append here: it would also fail parsing for every
# consumer that legitimately drops the RZ/V layers (AEN, NX91).
#
# drpai_1.4.0 is NOT listed there either: it is a headers-only recipe
# (${includedir}/linux/drpai.h) and belongs in DEPENDS, which alp-sdk's
# PACKAGECONFIG[drpai] already carries. The DRP-AI kernel driver itself
# is not a package either -- it is patched into linux-renesas by the
# layer's 0002-enable-drpai-driver.patch. mmngr{,buf}-user-module arrive
# via lib-tvm's own RDEPENDS.
#
# The "opted in without the layer" guard lives HERE rather than in the
# machine confs that own ALP_ENABLE_DRPAI, because bitbake's
# ConfHandler.feeder() accepts only data assignments, include/require,
# export, unset, unset-flag and addpylib -- an anonymous `python () { }`
# block in a .conf raises "ParseError: unparsed line: 'python () {'" and
# takes the whole machine down at parse time. A recipe is parsed by
# BBHandler, which does accept it. But that also means BitBake parses
# THIS recipe for every MACHINE BBFILES matches, not only the four RZ/V2N
# ones -- so the check below must gate itself on MACHINE, or a stale
# ALP_ENABLE_DRPAI = "1" left in a shared local.conf would bb.fatal() an
# e1m-nx9101-a55 (i.MX 93) or e1m-aen801-a32 (Alif Ensemble) parse too,
# even though neither machine conf defines ALP_ENABLE_DRPAI at all and
# neither wants the RZ layer.
#
# Note this is no longer "the" install site for lib-tvm, only the guard
# for it: the actual `IMAGE_INSTALL:append = "...lib-tvm..."` lives in
# each RZ/V2N machine conf (e1m-v2n101/102-a55.conf, e1m-v2m101/102-a55.conf),
# gated on the same ALP_ENABLE_DRPAI, and a MACHINE-level IMAGE_INSTALL:append
# reaches EVERY image built for that MACHINE -- alp-image-base,
# alp-image-prod, core-image-minimal -- not just alp-image-edge. The guard
# still belongs in a recipe (for the ConfHandler reason above), and this
# recipe is as good a place as any recipe that is guaranteed to parse
# whenever the machine confs do; it does not mean the install is scoped
# to this image.
#
# Gated on the 'rzv2n-family' MACHINEOVERRIDES override rather than on
# ALP_ENABLE_DRPAI's mere existence, because ALP_ENABLE_DRPAI is set with
# `?=` (a weak default) in the RZ/V2N confs -- checking only "is it set"
# can't distinguish a real RZ/V2N machine from one where some other layer
# happened to export the same name. 'rzv2n-family' is confirmed present at
# this parse point: all four RZ/V2N machine confs (e1m-v2n101-a55,
# e1m-v2n102-a55, e1m-v2m101-a55, e1m-v2m102-a55) `require
# conf/machine/rzv2n-evk.conf`, which `require`s
# conf/machine/include/rzv2n-family.inc (SOC_FAMILY = "rzv2n-family:
# mali-family"), which itself `require`s conf/machine/include/soc-family.inc
# -- the oe-core include that does
# `MACHINEOVERRIDES =. "${@['', '${SOC_FAMILY}:']['${SOC_FAMILY}' != '']}"`,
# i.e. prepends SOC_FAMILY when it is non-empty.
# All of that is machine-conf parsing, which BitBake finishes before it
# parses any recipe, so MACHINEOVERRIDES already carries 'rzv2n-family' by
# the time this anonymous python runs. Confirmed by reading
# meta-rz-bsp/conf/machine/rzv2n-evk.conf, .../include/rzv2n-family.inc
# and .../include/soc-family.inc directly, not assumed.
python () {
    if 'rzv2n-family' not in (d.getVar('MACHINEOVERRIDES') or '').split(':'):
        return
    if d.getVar('ALP_ENABLE_DRPAI') == '1' and \
       'rz-drpai' not in (d.getVar('BBFILE_COLLECTIONS') or '').split():
        bb.fatal('ALP_ENABLE_DRPAI = "1" but the rz-drpai layer '
                 '(meta-rz-drpai) is not in bblayers.conf -- lib-tvm and '
                 'kernel-module-mmngr do not exist without it. Add '
                 'meta-rz-drpai to bblayers.conf or set '
                 'ALP_ENABLE_DRPAI = "0".')
}
