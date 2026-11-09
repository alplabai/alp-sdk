# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)
project(gst-alpcamerasrc C)

# alpcamerasrc: GStreamer live source on <alp/camera.h> (CR10 -> GRAY8 /
# GRAY16_LE / NV12).  Linux/Yocto only; built by the gst-plugin-alp-camera
# recipe.  The pure conversion code is tested by
# tests/yocto/gst_alpcamerasrc_convert.c without GStreamer.
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-base-1.0 gstreamer-video-1.0)
find_library(ALP_SDK_LIB alp_sdk REQUIRED)
find_path(ALP_SDK_INC alp/camera.h REQUIRED)

add_library(gstalpcamera MODULE gstalpcamerasrc.c)
target_include_directories(gstalpcamera PRIVATE ${ALP_SDK_INC})
# GST_PLUGIN_DEFINE wants PACKAGE (the autotools plugins get it from config.h)
# The recipe passes -DALP_SDK_VERSION=${PV}.
set(ALP_SDK_VERSION "0.0.0" CACHE STRING "plugin version")
target_compile_definitions(gstalpcamera PRIVATE PACKAGE="alp-sdk"
                           ALP_PLUGIN_VERSION="${ALP_SDK_VERSION}")
target_link_libraries(gstalpcamera PRIVATE PkgConfig::GST ${ALP_SDK_LIB})

install(TARGETS gstalpcamera LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/gstreamer-1.0)
