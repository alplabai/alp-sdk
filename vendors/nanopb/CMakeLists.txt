# SPDX-License-Identifier: Apache-2.0
#
# vendors/nanopb -- nanopb integration anchor.  Publishes the
# include path for the stub headers.  When the real upstream
# nanopb is fetched via west the actual modules/lib/nanopb sources
# take precedence on the include path.
#
# Plain-CMake only -- the Zephyr build picks nanopb up via west.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/nanopb/CMakeLists.txt expects the alp_sdk target to be "
        "defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

target_include_directories(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
