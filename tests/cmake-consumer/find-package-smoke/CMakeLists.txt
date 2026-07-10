# SPDX-License-Identifier: Apache-2.0
#
# Regression fixture for issue #607: a MINIMAL standalone CMake project
# that consumes an INSTALLED alp-sdk the documented `find_package()` way
# -- as an external CMake consumer would, entirely outside the alp-sdk
# checkout, against a `cmake --install`ed prefix (not a build tree).
#
# Before the fix:
#   - CMakeLists.txt's `project(alp-sdk VERSION 0.1.0 ...)` was
#     hardcoded, so PROJECT_VERSION (and therefore
#     alp-sdk-config-version.cmake) never matched the real
#     `ALP_VERSION_STRING` (0.9.0) -- `find_package(alp-sdk 0.9
#     REQUIRED)` would have rejected a same-version install.
#   - Nothing ever installed the exported-targets file or the
#     generated config/version files (`install(EXPORT ...)` didn't
#     exist), so `find_package(alp-sdk CONFIG)` couldn't locate the
#     package at all.
#   - The package's CamelCase file names (AlpSdkConfig.cmake) never
#     matched find_package(alp-sdk)'s naming convention
#     (alp-sdk-config.cmake).
#
# Not part of the normal alp-sdk build: exercise it standalone, e.g.
#   cmake -B build/install -S . -DALP_OS=yocto -DALP_SDK_BUILD_SHARED=ON \
#       -DCMAKE_INSTALL_PREFIX=/tmp/alp-prefix
#   cmake --build build/install && cmake --install build/install
#   cmake -B build/consumer -S tests/cmake-consumer/find-package-smoke \
#       -DCMAKE_PREFIX_PATH=/tmp/alp-prefix
#   cmake --build build/consumer
#   ./build/consumer/find_package_smoke   # prints "alp-sdk version: 0.9.0"

cmake_minimum_required(VERSION 3.20)
project(alp_sdk_find_package_smoke LANGUAGES C)

find_package(alp-sdk 0.9 REQUIRED CONFIG)

add_executable(find_package_smoke main.c)
target_link_libraries(find_package_smoke PRIVATE alp::sdk)
