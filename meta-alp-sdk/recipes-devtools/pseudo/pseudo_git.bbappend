# meta-alp-sdk: bump pseudo-native past the GNU tar 1.35 / openat2 EFAULT.
#
# WHY: every A-cluster MACHINE's `base-files do_package` on a stock Ubuntu
# 24.04 build host was dying with:
#     tar: ./var/lib: Cannot mkdir: Bad address
# GNU tar 1.35 (Ubuntu 24.04 stock) creates its directory fd with the
# openat2(2) syscall. Vendored pseudo 1.9.0 only learns an fd's path when a
# WRAPPED libc symbol runs (pseudo_client.c:893-934); openat2 is not one of
# those wrapped symbols, so the fd never gets a tracked path. When tar then
# calls mkdirat() on that fd with a relative (NULL-after-lookup) path, the
# pseudo wrapper (pseudo_wrapfuncs.c:9632-9640 -> ports/unix/guts/mkdirat.c:21)
# forwards a NULL path straight to the real mkdirat() syscall, and the kernel
# correctly EFAULTs it. Confirmed by strace under a virgin
# PSEUDO_LOCALSTATEDIR:
#     openat2(3, "var/", {... RESOLVE_BENEATH}, 24) = 4
#     mkdirat(4, NULL, 0700)                        = -1 EFAULT (Bad address)
# This is not a kernel bug (openat2 has existed since Linux 5.6) and is not
# fixed by containerizing on 24.04 (same tar 1.35 either way).
#
# Upstream pseudo already fixed this exact failure -- it is the identical
# problem CVE-2025-45582 forced on Centos Stream's tar 1.34:
#   6533a53 "openat2: Implement openat2 wrapper"
#   472c897 "ports/linux/pseudo_wrappers: Avoid openat2 usage via syscall"
# and upstream poky's scarthgap branch has already picked up the fixed
# pseudo (SRCREV 823895ba708c63f6ae4dcbfc266210f26c02c698, PV 1.9.8),
# dropping the two local patches below because both landed upstream in the
# interim (no GLIBC_2_38 references remain on pseudo master). Verified by
# building pseudo-native under this bbappend: do_patch also needs the
# collateral fix below, everything else applies straight upstream --
# upstream already validated the combination on scarthgap, we're just
# pulling it across the vendor-snapshot boundary.
#
# WHY A BBAPPEND, NOT A POKY EDIT: our vendored poky
# (src_setup/yocto/poky, pulled in by the RZ/V2N AI SDK BSP) is a pinned,
# no-remote vendor snapshot. Per ADR 0017, alp-sdk rides OVER the vendor
# SDK and never rewrites it in place -- so the fix is an override from our
# own layer instead of an edit to poky's pseudo_git.bb.
#
# DELETION: this bbappend is a bridge. It is safe to delete the moment the
# vendor BSP ships a poky whose pseudo recipe already carries this SRCREV
# (or later).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRCREV = "823895ba708c63f6ae4dcbfc266210f26c02c698"
PV = "1.9.8"

# Both patches landed upstream between 28dcefb8 and 823895ba -- they no
# longer apply (and are unneeded) against the bumped SRCREV, so drop them
# from SRC_URI rather than let do_patch fail.
SRC_URI:remove = "file://0001-configure-Prune-PIE-flags.patch"
SRC_URI:remove = "file://glibc238.patch"

# older-glibc-symbols.patch (native/nativesdk-only, applied from pseudo.inc's
# SRC_URI:append:class-native / :class-nativesdk, so it is NOT in the SRC_URI
# list above and can't be dropped the same way) is a context-only rebase in
# upstream scarthgap: the bumped pseudo source added pseudo_client_scanf.o,
# which shifts the Makefile.in hunk context the patch matches against. The
# lines the patch actually adds/removes are unchanged. Rather than fork a
# 4th local patch, this ships upstream scarthgap's already-rebased copy of
# the same file (verbatim) so it applies against 823895ba; FILESEXTRAPATHS
# above makes bitbake find it here before the vendor poky's stale copy.
# See pseudo/older-glibc-symbols.patch in this directory.
