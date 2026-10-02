# SPDX-License-Identifier: Apache-2.0
#
# HE-side peer image for the mproc-mailbox flagship.  `board.yaml`
# declares this as the `m55_he` slice's `app:`, so `tan build`
# builds it alongside `../src` (m55_hp) as a real project core, not
# the topology-default `alp-stock-shim`.

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mproc_mailbox_peer LANGUAGES C)

target_sources(app PRIVATE main.c)
