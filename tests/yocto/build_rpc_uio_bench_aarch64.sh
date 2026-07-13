#!/usr/bin/env bash
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# alp-sdk #683 Path B, stage 5 -- cross-compile + statically link
# tests/yocto/rpc_uio_bench_main.c for the RZ/V2N A55 (aarch64 Linux
# userspace, no shared-lib deps) so a serial-only bench can transfer
# and run it directly against /dev/uio*.
#
# This is a HAND recipe, deliberately outside src/yocto/CMakeLists.txt's
# normal `pkg_check_modules(open-amp libmetal)` flow: that flow expects
# cross pkg-config .pc files describing an aarch64 open-amp/libmetal
# install, and none exist for a by-hand-assembled aarch64-linux-gnu
# sysroot (apt-get download + dpkg-deb -x, no real multiarch dpkg
# cross-support on the build host) -- see alp-sdk #683 Phase 3's
# provenance for that toolchain + the libopen_amp.a/libmetal.a it
# produced.  A future bench iteration that needs this cross-link
# repeatedly should fold it into a proper CMake toolchain file instead
# of hand-rolling `gcc` invocations; this script exists so today's
# exact, working recipe is reproducible without re-deriving it.
#
# Required environment (no defaults -- every path is host-specific):
#   ALP_AARCH64_TOOLCHAIN_ROOT  Root containing root/usr/bin/aarch64-linux-gnu-gcc
#                               (the sysroot itself lives at $ROOT/root)
#   ALP_AARCH64_OPENAMP_PREFIX  Prefix containing include/{metal,openamp}
#                               and lib/{libopen_amp.a,libmetal.a}
#
# Usage:
#   ALP_AARCH64_TOOLCHAIN_ROOT=/path/to/aarch64-toolchain \
#   ALP_AARCH64_OPENAMP_PREFIX=/path/to/aarch64-toolchain/install \
#     tests/yocto/build_rpc_uio_bench_aarch64.sh <output-path>
set -euo pipefail

if [[ -z "${ALP_AARCH64_TOOLCHAIN_ROOT:-}" || -z "${ALP_AARCH64_OPENAMP_PREFIX:-}" ]]; then
    echo "error: set ALP_AARCH64_TOOLCHAIN_ROOT + ALP_AARCH64_OPENAMP_PREFIX -- see this" >&2
    echo "       script's header comment for what each must contain." >&2
    exit 1
fi

out="${1:?usage: $0 <output-binary-path>}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tc="${ALP_AARCH64_TOOLCHAIN_ROOT}"
openamp="${ALP_AARCH64_OPENAMP_PREFIX}"

cc="${tc}/root/usr/bin/aarch64-linux-gnu-gcc"
sysroot="${tc}/root"
libsysfs_a="${sysroot}/usr/lib/aarch64-linux-gnu/libsysfs.a"

# The cross binutils this toolchain's `as`/`ld` depend on ship their own
# x86_64 libopcodes/libbfd/libctf (named "-arm64" for the TARGET they
# disassemble, not their own host arch) alongside it rather than into a
# system multiarch path -- without this on the loader path, `as` itself
# fails with "error while loading shared libraries" before ever reaching
# our source. See the same libs list under $tc/root/usr/lib/x86_64-linux-gnu.
export LD_LIBRARY_PATH="${sysroot}/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

"${cc}" \
    --sysroot="${sysroot}" \
    -static -static-libgcc \
    -std=gnu11 -O2 -Wall \
    -DALP_SDK_HAVE_OPENAMP_USERLAND=1 \
    -DCONFIG_ALP_SOC_RENESAS_RZV2N_N44=1 \
    -I"${repo_root}/include" \
    -I"${repo_root}/src" \
    -I"${repo_root}/src/common" \
    -I"${openamp}/include" \
    -pthread \
    "${repo_root}/tests/yocto/rpc_uio_bench_main.c" \
    "${repo_root}/src/rpc_dispatch.c" \
    "${repo_root}/src/backend.c" \
    "${repo_root}/src/status_strings.c" \
    "${repo_root}/src/backends/rpc/yocto_uio_drv.c" \
    "${openamp}/lib/libopen_amp.a" \
    "${openamp}/lib/libmetal.a" \
    "${libsysfs_a}" \
    -o "${out}"

"${tc}/root/usr/bin/aarch64-linux-gnu-strip" --strip-all "${out}"

echo "built: ${out}"
file "${out}" || true
