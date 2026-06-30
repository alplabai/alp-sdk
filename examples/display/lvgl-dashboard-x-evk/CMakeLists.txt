cmake_minimum_required(VERSION 3.20)
project(alp-lvgl-dashboard C)

# Linux-only example: links the distro's shared LVGL 9 (DRM backend).
#
# Build via the Yocto alp-lvgl-dashboard recipe (recommended) or
# cross-compile standalone against the SDK sysroot:
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
#         -S examples/display/lvgl-dashboard-x-evk -B build/dash
#   cmake --build build/dash
#
# LVGL must be built with PACKAGECONFIG = "drm" in meta-oe so that
# LV_USE_LINUX_DRM and LV_USE_EVDEV are compiled into the shared lib.
add_executable(alp-lvgl-dashboard src/main.c)

find_library(LVGL_LIB lvgl REQUIRED)
target_link_libraries(alp-lvgl-dashboard ${LVGL_LIB})

install(TARGETS alp-lvgl-dashboard RUNTIME DESTINATION bin)
