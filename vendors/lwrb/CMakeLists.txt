# SPDX-License-Identifier: Apache-2.0
#
# vendors/lwrb -- LwRB integration anchor.  Publishes the include
# path for the stub header.  When the real upstream LwRB is fetched
# via west the actual modules/lib/lwrb sources take precedence on
# the include path.
#
# Plain-CMake only -- the Zephyr build picks LwRB up via west.

if(NOT TARGET alp_sdk)
    message(FATAL_ERROR
        "vendors/lwrb/CMakeLists.txt expects the alp_sdk target to be "
        "defined by the parent CMakeLists.txt (plain-CMake mode only).")
endif()

target_include_directories(alp_sdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
