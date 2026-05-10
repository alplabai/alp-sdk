# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the alp-studio codegen helper, packaged for use
# on Yocto target devices (e.g. so a deployed unit can re-generate
# its own glue code from a saved .alp project, or so an in-field
# updater can regenerate after a model swap).
#
# alp-studio itself is GUI / cloud / browser; this recipe is just
# the command-line `alp-studio-codegen` binary that lives at
# https://github.com/alplabai/alp-studio's `codegen/` subtree.
#
# Most images don't need this on-device.  Include it only when
# you have a use case for regenerating on-target.

SUMMARY     = "ALP Studio codegen helper (command-line)"
DESCRIPTION = "Headless command-line tool that takes a .alp project file \
+ chip-metadata JSON and emits the C glue between user blocks and the \
ALP SDK runtime.  Useful for in-field regeneration on Yocto devices."
HOMEPAGE    = "https://github.com/alplabai/alp-studio"
LICENSE     = "Apache-2.0"
# License-checksum will pin to alp-studio's LICENSE once the repo is
# created; left as a placeholder for now.
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

SRC_URI = "git://github.com/alplabai/alp-studio.git;protocol=https;branch=main;destsuffix=git"
SRCREV  = "${AUTOREV}"
PV      = "0.1.0+git${SRCPV}"

# The repo isn't created yet; SKIP the recipe unless the user
# explicitly opts in via DISTRO_FEATURES.  Once alp-studio lands
# we can drop this guard.
python __anonymous() {
    if "alp-studio-codegen" not in d.getVar("DISTRO_FEATURES", True).split():
        raise bb.parse.SkipPackage("alp-studio repo not yet public; opt-in via DISTRO_FEATURES = \"... alp-studio-codegen\"")
}

S = "${WORKDIR}/git/codegen"

DEPENDS = "alp-sdk-runtime"

inherit cmake

EXTRA_OECMAKE = "-DCODEGEN_INSTALL_CLI=ON"

FILES:${PN}      = "${bindir}/alp-studio-codegen"
FILES:${PN}-data = "${datadir}/alp-studio/templates/"
