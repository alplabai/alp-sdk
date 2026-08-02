# SPDX-License-Identifier: Apache-2.0
#
# Top-level CMakeLists for the rpmsg-aen example -- multi-slice
# project root (linux/ + m55_hp/).  See README.md for the customer-
# facing build flow; `tan build` dispatches each slice to its own
# backend.

message(STATUS "rpmsg-aen: multi-slice project root "
               "(see linux/, m55_hp/, board.yaml).")
