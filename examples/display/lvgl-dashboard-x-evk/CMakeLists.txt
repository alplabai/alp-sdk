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

# Consume LVGL via pkg-config rather than a bare find_library(): meta-oe's
# lvgl.pc carries the -I for the staged lv_conf.h. lv_conf.h lives at
# include/lvgl/lv_conf.h, one level above include/lvgl/src/, so
# lv_conf_internal.h's fallback `#include "../../lv_conf.h"` (relative to
# src/) overshoots to /usr/lv_conf.h and never finds it. The -I from
# lvgl.pc plus LV_CONF_INCLUDE_SIMPLE below make LVGL look for lv_conf.h
# on the include path instead of guessing a relative path.
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LVGL IMPORTED_TARGET lvgl)
endif()

if(LVGL_FOUND)
    target_link_libraries(alp-lvgl-dashboard PkgConfig::LVGL)
else()
    # Fallback for the documented standalone/sysroot cross-compile path
    # above: a bare toolchain.cmake invocation may predate lvgl.pc, or
    # have no pkg-config wired into PATH/PKG_CONFIG_PATH at all. Locate
    # the library directly and add the same include dir pkg-config would
    # have supplied.
    find_library(LVGL_LIB lvgl REQUIRED)
    target_link_libraries(alp-lvgl-dashboard ${LVGL_LIB})
    target_include_directories(alp-lvgl-dashboard PRIVATE ${CMAKE_SYSROOT}/usr/include/lvgl)
endif()

# meta-oe's liblvgl.so calls into libdrm (lv_linux_drm.c, built in per the
# PACKAGECONFIG = "drm" note above) but its own lvgl.pc carries no
# `Requires`/`Libs.private` for libdrm, and the .so records no DT_NEEDED
# entry for it either -- so without an explicit link here, poky's default
# --no-allow-shlib-undefined leaves drmMode*/drmIoctl undefined at our
# final link. Link it directly rather than depending on LVGL's package to
# pull it in transitively.
if(PkgConfig_FOUND)
    pkg_check_modules(DRM IMPORTED_TARGET libdrm)
endif()
if(DRM_FOUND)
    target_link_libraries(alp-lvgl-dashboard PkgConfig::DRM)
else()
    find_library(DRM_LIB drm REQUIRED)
    target_link_libraries(alp-lvgl-dashboard ${DRM_LIB})
endif()

# Tell LVGL to pick up <lv_conf.h> from that include path instead of
# guessing a path relative to the lvgl/ source tree (see comment above).
target_compile_definitions(alp-lvgl-dashboard PRIVATE LV_CONF_INCLUDE_SIMPLE)

install(TARGETS alp-lvgl-dashboard RUNTIME DESTINATION bin)
