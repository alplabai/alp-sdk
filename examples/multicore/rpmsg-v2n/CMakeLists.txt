# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists for the rpmsg-v2n example.
#
# This example is a multi-slice project: each per-core sub-directory
# (linux/, m33_sm/) carries its own CMakeLists.txt and is dispatched
# to the right build backend by the orchestrator:
#
#   * `tan build examples/multicore/rpmsg-v2n` -- the canonical way; reads
#     board.yaml, fans out into per-core slices, emits the system
#     manifest.
#
#   * Direct `west build examples/multicore/rpmsg-v2n/m33_sm` also works for
#     iterating on the M33 half only (Zephyr-only build, no Yocto
#     side).
#
# This top-level file intentionally carries no add_subdirectory()
# calls because the slices use different build systems (Zephyr +
# plain CMake) that cannot share one configure step.  The
# orchestrator runs each slice in its own scoped subprocess.
#
# See README.md "Build" for the customer-facing flow.

# Marker so customers / CI can `cmake -P` this file as a sanity
# check that the directory is a recognised alp-sdk multi-slice
# project root.
message(STATUS "rpmsg-v2n: multi-slice project root "
               "(see linux/, m33_sm/, board.yaml).")
