# SPDX-License-Identifier: Apache-2.0
#
# Multi-slice project root marker (linux/ = plain CMake, m33_sm/ = Zephyr).
# Same shape as ../rpmsg-v2n/CMakeLists.txt: the slices use different build
# systems, so the orchestrator (`tan build`) builds each one in its own
# subprocess and this file carries no add_subdirectory().
message(STATUS "microros-ros2-v2n: multi-slice project root "
               "(see linux/, m33_sm/, board.yaml).")
