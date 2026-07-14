#!/usr/bin/env bash
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# GHSA-xhm8-7f87-93q5 defect 3: this directory's RPC close-path
# regression tests (rpc_yocto_self_close.c, rpc_dispatch_close_race.c)
# only proved "no double-free" / "no data race" when a developer
# happened to build them under ASan/UBSan/TSan by hand -- the plain
# CTest registration below built + ran them un-instrumented.  This
# script is what tests/yocto/CMakeLists.txt wires into an ACTUAL CTest
# target (alp_test_rpc_asan_ubsan / alp_test_rpc_tsan) so the sanitizer
# proof runs in CI every time, not by hand.
#
# Why a full, separate reconfigure (not per-target sanitizer flags on
# just the test executables): alp_test_rpc_dispatch_close_race links
# the REAL src/rpc_dispatch.c through the alp::sdk static library --
# for ThreadSanitizer (or ASan's stack/global instrumentation) to see
# anything happening inside that code, alp_sdk itself must be compiled
# with the same -fsanitize flags, not just the test's own translation
# unit.  CMAKE_C_FLAGS / CMAKE_EXE_LINKER_FLAGS are project-global, so
# this configures + builds a completely separate, scratch copy of the
# whole tree with them set, rather than trying to selectively
# instrument one archive member (which risks ODR clashes with the
# already-built plain copy in the CALLING build directory).
#
# Usage:
#   run_sanitized_rpc_tests.sh <source_dir> <scratch_build_dir> \
#                              <sanitize_flags> <target>...
set -euo pipefail

source_dir="$1"; shift
build_dir="$1"; shift
sanitize_flags="$1"; shift
targets=("$@")

cmake -S "${source_dir}" -B "${build_dir}" \
    -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="${sanitize_flags} -g -O1" \
    -DCMAKE_EXE_LINKER_FLAGS="${sanitize_flags}" \
    >/dev/null

nproc_bin="$(command -v nproc || true)"
jobs=4
if [ -n "${nproc_bin}" ]; then
    jobs="$("${nproc_bin}")"
fi

cmake --build "${build_dir}" --target "${targets[@]}" -j"${jobs}"

# ThreadSanitizer's fixed shadow-memory layout is incompatible with the
# ASLR entropy some sandboxed/containerised hosts (and modern kernels'
# larger mmap_rnd_bits default) apply by default -- observed here as
# "FATAL: ThreadSanitizer: unexpected memory mapping ...", not a bug in
# the code under test.  `setarch -R` (disable ASLR) is the standard,
# well-documented workaround; it is a no-op wrapper on a host that
# doesn't need it, so it is always safe to apply.
runner=()
if [[ "${sanitize_flags}" == *"thread"* ]] && command -v setarch >/dev/null 2>&1; then
    runner=(setarch "$(uname -m)" -R)
fi

status=0
for t in "${targets[@]}"; do
    echo "==> running ${t} under: ${sanitize_flags}"
    if ! "${runner[@]}" "${build_dir}/tests/yocto/${t}"; then
        status=1
    fi
done
exit "${status}"
