# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# PYTHON_EXECUTABLE is only set after find_package(Zephyr);
# we need a Python interpreter at configure time *before*
# that, so resolve one ourselves here.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

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
# Emit into a generated/ subdir so the file escapes Zephyr's
# ${APPLICATION_BINARY_DIR}/*.conf GLOB, and APPEND (don't overwrite)
# OVERLAY_CONFIG so the per-example native_sim.conf can also layer in.
set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${_alp_generated}
            --emit zephyr-conf --core m33_sm
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(audio_noise_suppression LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
)
