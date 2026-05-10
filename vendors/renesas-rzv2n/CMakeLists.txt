# SPDX-License-Identifier: Apache-2.0
#
# vendors/renesas-rzv2n -- Renesas RZ/V2N FSP bindings for the
# bare-metal V2N backend.  Pulled in only when the build sets:
#
#     ALP_OS=baremetal  AND  ALP_SOM=v2n
#
# Same shape as vendors/alif/ (see CMakeLists.txt + i2c.c there
# for the pattern).  ALP_HAS_RENESAS_FSP gates the real bodies;
# without it the wrapper sources compile to empty translation
# units and stub_backend.c keeps the NOSUPPORT defaults.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/renesas-rzv2n/CMakeLists.txt expects the alp_sdk target to be defined "
        "by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

option(ALP_HAS_RENESAS_FSP
    "Link the bare-metal V2N backend against Renesas FSP."
    OFF)

target_sources(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/i2c.c
    ${CMAKE_CURRENT_SOURCE_DIR}/spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/uart.c
)

if(ALP_HAS_RENESAS_FSP)
    target_compile_definitions(alp_sdk PRIVATE
        ALP_HAS_RENESAS_FSP=1
        ALP_VENDOR_OVERRIDES_PERIPHERAL=1)
    # FSP itself is found via Renesas's pack distribution at
    # $ENV{FSP_PACK_ROOT}; downstream apps add the include dirs +
    # link the driver libraries.  v0.4 wires this from CI.
endif()
