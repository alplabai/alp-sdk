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

# alp-drpai-inference (the issue #1268 exhibition-booth demo) rides the
# SAME opt-in this MACHINE's ALP_ENABLE_DRPAI switch drives (see e.g.
# e1m-v2n101-a55.conf), but installed here rather than there -- an
# IMAGE-level append, so this booth demo reaches alp-image-edge only,
# never alp-image-prod. The IMAGE-level install SHAPE mirrors the
# alp-lvgl-dashboard precedent above; the ALP_ENABLE_DRPAI gate itself
# does not -- alp-lvgl-dashboard installs unconditionally, and
# ALP_ENABLE_DRPAI alone is not a sufficient gate here either: it is
# ONLY declared (`?=`) in the six RZ/V2N machine confs, so this line
# also checks 'rzv2n-family' in MACHINEOVERRIDES, the same guard the
# python() block below uses, so a stray ALP_ENABLE_DRPAI = "1" in a
# shared local.conf cannot pull this RZ-only recipe into an
# e1m-nx9101-a55 / e1m-aen801-a32 build (see that guard's comment for
# why 'rzv2n-family' is confirmed present at this parse point).
IMAGE_INSTALL += "${@' alp-drpai-inference' if d.getVar('ALP_ENABLE_DRPAI') == '1' and \
    'rzv2n-family' in (d.getVar('MACHINEOVERRIDES') or '').split(':') else ''}"

# NOTE: the DRP-AI userspace RUNTIME PACKAGES (lib-tvm +
# kernel-module-mmngr) and the SDK BACKEND compiled into libalp_sdk.so
# are two separate, independent concerns -- see
# docs/bring-up-drpai-v2n.md section 4 for the full two-switch contract:
#   1. alp-image-common.inc's ALP_RZ_DRPAI_INSTALL (issue #1176) is the
#      SOLE install site for the runtime packages, and installs them on
#      every alp-image-* image (alp-image-base/-edge/-prod -- the three
#      recipes that `require alp-image-common.inc`) whenever the
#      rz-drpai layer is in BBFILE_COLLECTIONS AND 'v2n' is in
#      MACHINE_FEATURES, independent of ALP_ENABLE_DRPAI. A non-alp
#      core-image-* build with ALP_ENABLE_DRPAI = "1" does NOT get this
#      pair from alp-sdk's tree at all -- it needs its own install, or
#      the vendor layer's own core-image bbappend (no such bbappend is
#      present in this tree).
#   2. `PACKAGECONFIG[drpai]` on the alp-sdk recipe compiles the SDK
#      backend in; it installs no userspace package and neither this
#      file nor alp-image-common.inc touches it.
#
# drpai_1.4.0 is NOT covered by mechanism 1 above either: it is a
# headers-only recipe (${includedir}/linux/drpai.h) and belongs in
# DEPENDS, which alp-sdk's PACKAGECONFIG[drpai] already carries. The
# DRP-AI kernel driver itself is not a package either -- it is patched
# into linux-renesas by the layer's 0002-enable-drpai-driver.patch.
# mmngr{,buf}-user-module arrive via lib-tvm's own RDEPENDS.
#
# meta-rz-codecs / meta-rz-opencva (hardware video codec, OpenCV-DRP
# accel) are the other two members of the meta-rz-drpai/codecs/opencva
# trio (issue #1176); this image enables the alp-camera FEATURE (see
# IMAGE_FEATURES above), so their RDEPENDS payload rides in via
# packagegroup-alp-camera.bb the same way it does for alp-image-prod.bb
# -- see that file's own NOTE for the full rationale and the
# vendor-bbappend exclusions.
#
# The "opted in without the layer" guard lives HERE rather than in the
# machine confs that own ALP_ENABLE_DRPAI, because bitbake's
# ConfHandler.feeder() accepts only data assignments, include/require,
# export, unset, unset-flag and addpylib -- an anonymous `python () { }`
# block in a .conf raises "ParseError: unparsed line: 'python () {'" and
# takes the whole machine down at parse time. A recipe is parsed by
# BBHandler, which does accept it. But that also means BitBake parses
# THIS recipe for every MACHINE BBFILES matches, not only the six RZ/V2N
# ones -- so the check below must gate itself on MACHINE, or a stale
# ALP_ENABLE_DRPAI = "1" left in a shared local.conf would bb.fatal() an
# e1m-nx9101-a55 (i.MX 93) or e1m-aen801-a32 (Alif Ensemble) parse too,
# even though neither machine conf defines ALP_ENABLE_DRPAI at all and
# neither wants the RZ layer.
#
# lib-tvm's install site is alp-image-common.inc alone (see the NOTE
# above), on every alp-image-* image; no RZ/V2N machine conf installs
# it. The guard below still belongs in a recipe (for the ConfHandler
# reason above), and this recipe is as good a place as any recipe that
# is guaranteed to parse whenever the machine confs do; it protects the
# alp-drpai-inference install above (and the DT node's own
# ALP_DRPAI_LAYER gate) from a misconfigured ALP_ENABLE_DRPAI = "1"
# with no rz-drpai layer present -- not a packaging install site of its
# own.
#
# Gated on the 'rzv2n-family' MACHINEOVERRIDES override rather than on
# ALP_ENABLE_DRPAI's mere existence, because ALP_ENABLE_DRPAI is set with
# `?=` (a weak default) in the RZ/V2N confs -- checking only "is it set"
# can't distinguish a real RZ/V2N machine from one where some other layer
# happened to export the same name. 'rzv2n-family' is confirmed present at
# this parse point: all six RZ/V2N machine confs (e1m-v2n101-a55,
# e1m-v2n102-a55, e1m-v2n103-a55, e1m-v2m101-a55, e1m-v2m102-a55,
# e1m-v2m103-a55) `require
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
                 '(meta-rz-drpai) is not in bblayers.conf -- without it, '
                 'ALP_DRPAI_LAYER stays 0 and the &drpai0 devicetree '
                 'node falls back silently to its comment-only stub '
                 '(linux-renesas_%.bbappend), so the NPU never claims '
                 'its reserved memory, and alp-drpai-inference (if '
                 'pulled in above) has no runtime to run against. Add '
                 'meta-rz-drpai to bblayers.conf or set '
                 'ALP_ENABLE_DRPAI = "0".')
}
