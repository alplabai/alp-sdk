# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# board.yaml -> build/generated/alp.conf at configure time, layered
# over prj.conf via EXTRA_CONF_FILE.  Both the emit and the append
# MUST happen before find_package(Zephyr) -- cmake/alp.cmake documents
# why, and the ALP_SDK_ROOT contract a copied-out project relies on.
if(DEFINED ENV{ALP_SDK_ROOT})
    set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
else()
    get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
endif()
include(${ALP_SDK_ROOT}/cmake/alp.cmake)

# Resolve board.yaml -> alp.conf via the §D.lib loader.  This
# emits CONFIG_ALP_SDK_* knobs for the audio path:
#   - peripherals.i2s            -> CONFIG_I2S=y
#   - libraries.tflite_micro     -> CONFIG_ALP_TFLM_REF_KERNELS=y
#   - libraries.cmsis_dsp        -> CONFIG_CMSIS_DSP=y
#   - board.populated.tas2563  -> CONFIG_ALP_SDK_CHIP_TAS2563=y
# Inference dispatchers compile in from the SoM preset's
# `capabilities:` block; the app picks per-handle at runtime via
# `alp_inference_open(.backend = ...)`.
# The audio loop is mic-in (TAS2563 over I2S) -> <alp/dsp.h>
# preprocess (FFT bin -> noise-floor estimate) -> <alp/inference.h>
# denoiser -> speaker-out (TAS2563 over the same I2S link).

alp_sdk_zephyr_conf(m33_sm)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_noise_suppression LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
)
