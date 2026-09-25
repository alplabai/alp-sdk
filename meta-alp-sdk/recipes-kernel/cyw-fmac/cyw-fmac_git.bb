# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the Murata cyw-fmac Wi-Fi backports driver on the
# Alp Lab E1M V2N / V2M SoM family (Infineon/Cypress CYW55513, Murata
# Type 2FY module, SDIO WLAN).
#
# This is a backports tree, NOT an in-tree kernel module: it ships its
# own Kconfig/gentree build system (Makefile -> Makefile.real ->
# Makefile.build) plus a vendored cfg80211/compat layer, so the stock
# obj-m flow in module.bbclass does not apply. do_configure runs the
# tree's `defconfig-brcmfmac`, and do_compile/do_install drive the
# tree's own `modules` / kernel `modules_install` targets with the OE
# cross toolchain. The resulting .ko set is:
#     compat.ko cfg80211.ko brcmutil.ko brcmfmac.ko
#
# KERNEL CONFIG CONTRACT (staged via the linux-renesas bbappend's
# wifi-bt.cfg fragment): the target kernel MUST build with
# CONFIG_CFG80211=m and the in-tree CONFIG_BRCMFMAC off. The backports
# cfg80211.ko/brcmfmac.ko install under the `updates/` modules subdir,
# which depmod prefers over the in-tree cfg80211 module -- so the
# in-tree BRCMFMAC must be off (it lacks the CYW55513 chip ID) and the
# in-tree cfg80211 must be a module (NOT built-in =y), otherwise the
# backports cfg80211 cannot shadow it. Backports also will not build at
# all with cfg80211 fully absent: net_device.ieee80211_ptr is
# CFG80211-guarded.

SUMMARY     = "Murata cyw-fmac (brcmfmac backports) Wi-Fi driver for CYW55513"
DESCRIPTION = "Out-of-tree Broadcom/Cypress FullMAC Wi-Fi backports driver \
(cfg80211 + brcmfmac) for the Infineon CYW55513 / Murata Type 2FY SDIO module \
on the Alp Lab E1M V2N / V2M SoM. Backports base v6.1.97; provides the \
CYW55500 chip-family ID (0xD8CC) absent from the in-tree 6.1.x brcmfmac."
HOMEPAGE    = "https://github.com/murata-wireless/cyw-fmac"
SECTION     = "kernel/modules"

LICENSE          = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

# Murata cyw-fmac backports, branch imx-mickledore-jaculus (v6.1.97 base,
# content-identical to master at this SRCREV). Two kernel-version
# backport shims layered on top -- see the patch headers.
SRC_URI = " \
    git://github.com/murata-wireless/cyw-fmac;protocol=https;branch=imx-mickledore-jaculus \
    file://0001-brcmfmac-sdio-include-uapi-sched-types-for-6.1.patch \
    file://0002-backport-genetlink-genlmsg_multicast_allns-4-arg-shim.patch \
"
SRCREV = "649b8c15e969ceca698531b1310a9ca563d1c2f9"
PV     = "6.1.97+git${SRCPV}"

S = "${WORKDIR}/git"

inherit module

# module.bbclass pulls in virtual/kernel via module-base; no explicit
# DEPENDS on the kernel is needed. The backports tree's own kconf
# defconfig step (do_configure's `defconfig-brcmfmac`) builds a small
# lex/yacc-generated config parser (kconf/Makefile: conf.o from
# zconf.lex.c/zconf.tab.c, built via $(LEX)/$(YACC)) -- neither host
# tool rides in on module.bbclass or on the kernel-yocto native
# sysroot, so both must be DEPENDS here explicitly (caught by a real
# `bitbake -c compile` dry-run 2026-09-25).
#
# flex-native's own sysroot only ships a `flex` binary, no `lex` name
# (no ALTERNATIVE-style compat symlink the way some distros' flex
# packages provide one) -- the Makefile's default $(LEX) is the literal
# string "lex", which isn't on PATH, so it must be overridden to
# "flex" explicitly (same class of fix for $(YACC) -> "bison -y",
# bison-native's own compat mode for a yacc-style invocation).
DEPENDS += "flex-native bison-native"

# The backports Makefile passes ARCH / CROSS_COMPILE through to the
# kernel build as make ARGS (it does not honour them from the
# environment for the inner kernel make), and it locates the target
# kernel via KLIB / KLIB_BUILD. KMODDIR=updates is the backports
# default install subdir; keep it so depmod prefers these modules over
# the in-tree cfg80211. LEX=flex is the flex-native override from the
# comment above -- kept in this SAME assignment (not a separate `+=`)
# because this is a plain `=`, not a `:append`, and a later plain
# assignment would silently discard an earlier `+=`. YACC needs no
# override: bison-native's sysroot already ships a `yacc` compat shim
# (unlike flex-native's, which has no `lex` name), so make's built-in
# default YACC=yacc already resolves.
# Intentional clobber of module.bbclass's default KERNEL_SRC: backports drives
# everything via KLIB/KLIB_BUILD; the class's KERNEL_SRC variable is unused here.
EXTRA_OEMAKE = " \
    ARCH=${ARCH} \
    CROSS_COMPILE=${TARGET_PREFIX} \
    KLIB_BUILD=${STAGING_KERNEL_BUILDDIR} \
    KLIB=${STAGING_KERNEL_DIR} \
    KMODDIR=updates \
    LEX=flex \
"

# module.bbclass exports only do_compile + do_install; do_configure is
# the base no-op, so override it fully here. Two steps:
#  1) restore the exec bit on the tree's shell helpers (scripts/*, the
#     lxdialog check), which the OE fetch/unpack/patch pipeline strips and
#     which kconf shells out to during defconfig;
#  2) generate the brcmfmac defconfig (.config + backport autoconf.h)
#     against the target kernel. ARCH/CROSS must be present here too or
#     the kconf kernel-version probe picks the host arch.
#
# CC=${BUILD_CC} on THIS invocation only (not folded into the shared
# EXTRA_OEMAKE above, which module_do_compile also uses): kconf/Makefile
# has no host/target split for its own `conf` binary -- it inherits
# whatever CC the CROSS_COMPILE prefix implies from Makefile.real, so
# without this override `conf` gets cross-compiled for the TARGET
# (aarch64) even though defconfig-brcmfmac runs it on the BUILD host to
# generate the .config. An unqualified cross-built `conf` fails with a
# shell trying (and failing) to execute an aarch64 ELF as a script --
# caught by a real `bitbake -c compile` run 2026-09-25 ("Syntax error:
# '(' unexpected"). do_compile's own module build never touches kconf/
# again, so this override has nothing to leak into there.
do_configure() {
    chmod -R +x ${S}/scripts/
    chmod +x ${S}/kconf/lxdialog/check-lxdialog.sh
    oe_runmake \
        ARCH=${ARCH} \
        CROSS_COMPILE=${TARGET_PREFIX} \
        KLIB_BUILD=${STAGING_KERNEL_BUILDDIR} \
        KLIB=${STAGING_KERNEL_DIR} \
        CC="${BUILD_CC}" \
        HOSTCC="${BUILD_CC}" \
        defconfig-brcmfmac
}

# Build via the backports `modules` target (not the kernel obj-m flow).
module_do_compile() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    oe_runmake \
        ARCH=${ARCH} \
        CROSS_COMPILE=${TARGET_PREFIX} \
        KLIB_BUILD=${STAGING_KERNEL_BUILDDIR} \
        KLIB=${STAGING_KERNEL_DIR} \
        KMODDIR=updates \
        modules
}

# Install the four .ko under .../modules/<ver>/updates/ in ${D}. We do
# NOT use the backports `install`/`modules_install` target: it re-runs
# the kernel modules_install with INSTALL_MOD_PATH=$KLIB and then fires
# host-side depmod / update-initramfs / blacklist scripts that have no
# place in a cross sysroot. Instead drive the kernel's own
# modules_install directly.
#
# MODLIB, not INSTALL_MOD_PATH/INSTALL_MOD_DIR: the kernel Makefile's
# modules_install rule hardcodes the "lib/modules/$(KERNELRELEASE)"
# path component under INSTALL_MOD_PATH -- literal "lib", regardless of
# this distro's nonarch_base_libdir (usrmerge: /usr/lib). module.bbclass's
# own default do_install sidesteps that the same way, by overriding
# MODLIB directly instead of relying on INSTALL_MOD_PATH's concatenation.
# Skipping this landed the .ko set under a literal /lib/modules/... in
# ${D} while kernel-module-split.bbclass's do_split_packages scans
# ${nonarch_base_libdir}/modules (/usr/lib/modules here) -- the two
# never matched, so do_package failed QA with "installed but not
# shipped" on all four .ko (caught by a real `bitbake cyw-fmac` run
# 2026-09-25). MODLIB's own trailing /updates keeps the depmod-shadow
# subdir the rest of this recipe's rationale depends on.
module_do_install() {
    unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
    oe_runmake -C ${STAGING_KERNEL_BUILDDIR} \
        M=${S} \
        ARCH=${ARCH} \
        CROSS_COMPILE=${TARGET_PREFIX} \
        MODLIB=${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}/updates \
        DEPMOD=true \
        modules_install
}

# kernel-module-split's default package-name template is
# "kernel-module-<basename>-${KERNEL_VERSION}", with NO recipe-name
# component -- so a bare ".ko" basename collides across recipes. cfg80211
# is exactly that case here: our own wifi-bt.cfg leaves the in-tree
# CONFIG_CFG80211=m (see that file's header for why), so linux-renesas
# ALSO ships a literal "kernel-module-cfg80211-<kernelversion>" package
# -- identical to the one this recipe's own cfg80211.ko would produce.
# do_packagedata hard-fails on that ("trying to install files into a
# shared area when those files already exist"), caught by a real
# `bitbake cyw-fmac` run 2026-09-25 (compat/brcmutil/brcmfmac don't
# collide today -- BRCMFMAC is off in-tree so brcmutil/brcmfmac aren't
# built there either -- but prefixing all four uniformly is simpler to
# reason about than tracking which basenames happen to be safe this
# kernel config, and stays safe if that ever changes).
KERNEL_MODULE_PACKAGE_PREFIX = "cyw-fmac-"

# PACKAGES_DYNAMIC is what lets an RDEPENDS on a not-yet-computed
# per-.ko split package resolve at all: kernel.bbclass declares
# "^${KERNEL_PACKAGE_NAME}-module-.*" for the IN-TREE kernel recipe, but
# that wildcard doesn't match our own cyw-fmac- prefixed names, so the
# RDEPENDS below would otherwise fail dependency resolution with
# "Nothing RPROVIDES" before a single task ever runs (caught by a real
# `bitbake cyw-fmac` run 2026-09-25).
PACKAGES_DYNAMIC += "^cyw-fmac-kernel-module-.*"

# Pull the whole .ko set (compat, cfg80211, brcmutil, brcmfmac) into the
# meta package so MACHINE_EXTRA_RRECOMMENDS += "cyw-fmac" installs all
# four. kernel-module-split auto-splits each .ko into its own
# cyw-fmac-kernel-module-<name> package (per the PREFIX above); the
# empty ${PN} meta-package RDEPENDS on the version-independent virtual
# names (KERNEL_MODULE_PROVIDE_VIRTUAL default =1 strips the
# ${KERNEL_VERSION} suffix off each split package's own RPROVIDES),
# which is what the machine-conf RRECOMMENDS targets.
RDEPENDS:${PN} += " \
    cyw-fmac-kernel-module-compat \
    cyw-fmac-kernel-module-cfg80211 \
    cyw-fmac-kernel-module-brcmutil \
    cyw-fmac-kernel-module-brcmfmac \
"

# NOTE on the cyw-fmac-kernel-module-brcmfmac / -cfg80211 names:
# kernel-module-split already produces those split packages from this
# recipe's own .ko set, so an RPROVIDES of the same names on ${PN} would
# be a same-recipe collision and is deliberately omitted. The
# shadow-over-in-tree MODULE-LOADING behaviour (which .ko depmod picks)
# comes from the updates/ subdir depmod priority (do_install above) plus
# the kernel built with BRCMFMAC off -- NOT from these package names;
# the KERNEL_MODULE_PACKAGE_PREFIX above exists purely to keep the OE
# PACKAGE metadata from colliding with in-tree's. KERNEL_MODULE_PACKAGE_SUFFIX
# keeps its default (-${KERNEL_VERSION}) so packages version with the
# kernel they built against.

COMPATIBLE_MACHINE = "e1m-v2n101-a55|e1m-v2n102-a55|e1m-v2n103-a55|e1m-v2m101-a55|e1m-v2m102-a55|e1m-v2m103-a55"
