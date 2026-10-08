# SPDX-License-Identifier: Apache-2.0
#
# Production image -- hardened, customer-facing. Builds on the headless
# alp-image-base (alp-image-common) and enables the vision-appliance feature
# groups (camera + display) but NOT ros -- a shipped unit shouldn't carry the
# ROS 2 + DDS stack unless the customer's app needs it (add alp-ros then).
# Production posture vs the edge image:
#   - NO debug-tweaks: root is not passwordless and console root login is
#     locked (no empty-password autologin).
#   - SSH is key-only (alp-ssh-hardening): PermitRootLogin prohibit-password +
#     PasswordAuthentication no. The per-unit authorized key is installed at
#     provisioning time (scripts/provision_som.py), never baked into the image.
#   - Developer tooling and zero-conf/telephony/RPC discovery daemons trimmed.
#
# Build against the ALP distro so the image carries the rebranded identity
# (not the "Poky ... Reference Distro" banner):
#   DISTRO=alp MACHINE=e1m-v2m101-a55 bitbake alp-image-prod

SUMMARY = "Alp SDK production image (hardened: key-only SSH, no debug tooling)"

require alp-image-common.inc

# Vision appliance: camera + display, but NOT ros (opt in with alp-ros if the
# product's app is a ROS node). debug-tweaks is never enabled here.
IMAGE_FEATURES += "alp-camera alp-display"

# Strip developer features even when the vendor build template's
# EXTRA_IMAGE_FEATURES (local.conf) injects them image-wide:
#   debug-tweaks  = empty root password + passwordless login
#   tools-profile = profilers
#   tools-testapps= test apps (also drags connman -- see alp-image-common)
#   rz-vlp-tools  = the Renesas VLP tools umbrella. Its packagegroups
#                   HARD-RDEPEND (not recommend) a development/debug/benchmark
#                   bundle that has no place on a shipped unit: tcf-agent (the
#                   Eclipse remote-debug daemon), libdrm-tests, fio/bonnie++/
#                   iperf3/memtester, minicom/ckermit/can-utils/yavta, etc.
#                   Because they are hard deps, excluding them individually
#                   makes do_rootfs fail ("conflicting requests") -- the umbrella
#                   feature itself has to go.
IMAGE_FEATURES:remove = "debug-tweaks tools-profile tools-testapps rz-vlp-tools"

# rz-vlp-tools-base also carried the ext4 fsck tools (e2fsprogs); the product
# rootfs is ext4, so keep e2fsck explicitly for power-loss recovery.
IMAGE_INSTALL += " e2fsprogs-e2fsck"

# Key-only SSH + locked root (an sshd_config.d drop-in). The public key itself
# is provisioned per unit, never baked in.
IMAGE_INSTALL += " alp-ssh-hardening"

# Drop discovery/telephony/RPC daemons that core packagegroups pull in only as
# RRECOMMENDS -- unwanted on a fixed-function, networkd-only appliance reached
# at a fixed address. (tcf-agent is NOT here: it is a hard dep of rz-vlp-tools,
# removed above -- excluding a hard dep would break the rootfs.)
BAD_RECOMMENDATIONS += " \
    avahi-daemon \
    connman \
    ofono \
    rpcbind \
"

# meta-rz-drpai / meta-rz-codecs / meta-rz-opencva vendor payload (DRP-AI
# NPU runtime, hardware video codec, OpenCV-DRP accel) is NOT wired here.
# DRP-AI (+ the SDK sysroot headers for all three) is core payload wired
# once in alp-image-common.inc; the codecs/opencva RDEPENDS payload this
# image's camera feature needs rides the alp-camera FEATURE (enabled
# above) via packagegroup-alp-camera.bb's own RDEPENDS -- see those two
# files for the full rationale (issue #1176) and the vendor-bbappend
# exclusions.

# Production units keep CM33 SRAM secure (decision Q53).  ALP_V2N_CM33_SRAM_NS
# = "1" is the dev-only flag that lets a Linux root process rewrite CM33 code
# memory (non-secure TZC access + remoteproc stop/reload); refuse to build a
# shipped image with it.  The TF-A it controls is a separate recipe, so also
# keep the flag out of any local.conf used for production builds. Skip (not
# bb.fatal) so a dev local.conf with the flag can still parse and build every
# other recipe; `bitbake alp-image-prod` then reports this reason and stops.
python () {
    if d.getVar('ALP_V2N_CM33_SRAM_NS') == '1':
        raise bb.parse.SkipRecipe('ALP_V2N_CM33_SRAM_NS = "1" makes CM33 code memory writable '
                                  'from Linux: dev images only, never alp-image-prod')
}
