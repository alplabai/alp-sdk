# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-inference -- on-silicon FULL NPU inference on the E1M-AEN801 (Ensemble
# E8, M55-HE).  A Vela-compiled int8 model runs through TFLM's MicroInterpreter,
# whose Ethos-U custom-op resolver dispatches the fused subgraph onto the
# Ethos-U85 (256-MAC) NPU via the Arm Ethos-U core driver (ethosu_invoke).
#
# This is the inference follow-up to aen-npu-ethosu-regcheck (which proved the
# U85 is present + clocked).  It mirrors the upstream tflm_ethosu sample's
# end-to-end pattern: Vela-compile -> embed the network as a C array -> TFLM
# MicroInterpreter + MicroMutableOpResolver::AddEthosU() -> Invoke().
#
# Modules required at build time (pass via -DEXTRA_ZEPHYR_MODULES):
#   /home/caner/alp-sdk                         (the SDK; provides ALP_SDK Kconfig)
#   /home/caner/modules/hal/alif                (hal_alif -- SoC support)
#   /home/caner/zephyrproject/modules/hal/ethos_u  (Arm Ethos-U core driver lib)
#   <tflite-micro path>                         (TFLM sources + Zephyr glue)
#
# The model-gen step (gen_model.py) runs vela from .venv-vela and writes
# model.h into the build tree, so the source tree carries no generated blob.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_inference LANGUAGES C CXX)

# --------------------------------------------------------------------------
# Model-gen: Vela-compile the fixture for the U85 and emit model.h.
# --------------------------------------------------------------------------
# Defaults target the in-tree FIXTURE model (tiny_int8.tflite).  Override
# -DAEN_NPU_MODEL=<path> on the command line to point at a larger REAL int8
# model (e.g. a person-detect / mobilenet) once one is staged in-tree; the
# RESULT contract in main.c is model-agnostic.
set(AEN_NPU_VELA   "${CMAKE_CURRENT_SOURCE_DIR}/../../../.venv-vela/bin/vela"
    CACHE FILEPATH "Path to the Vela compiler executable")
set(AEN_NPU_ACCEL  "ethos-u85-256"
    CACHE STRING   "Vela --accelerator-config (the AEN801 U85 is 256-MAC)")
set(AEN_NPU_MODEL  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fixtures/models/tiny_int8.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "tiny_int8_fixture"
    CACHE STRING   "Embedded model identity string")

set(AEN_NPU_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/model-gen")
set(AEN_NPU_MODEL_H "${AEN_NPU_GEN_DIR}/model.h")

# Run at configure time so the header exists before the first compile, and
# again as a build rule so editing the input model / gen script re-runs it.
add_custom_command(
	OUTPUT  ${AEN_NPU_MODEL_H}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
	        --vela       ${AEN_NPU_VELA}
	        --accel      ${AEN_NPU_ACCEL}
	        --model      ${AEN_NPU_MODEL}
	        --name       ${AEN_NPU_MODEL_NAME}
	        --build-dir  ${AEN_NPU_GEN_DIR}
	        --out        ${AEN_NPU_MODEL_H}
	DEPENDS ${AEN_NPU_MODEL} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
	COMMENT "aen-npu-inference: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL}"
	VERBATIM
)

# Configure-time generation (so the include path resolves on the first build).
if(NOT EXISTS ${AEN_NPU_MODEL_H})
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
		COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
		        --vela ${AEN_NPU_VELA} --accel ${AEN_NPU_ACCEL}
		        --model ${AEN_NPU_MODEL} --name ${AEN_NPU_MODEL_NAME}
		        --build-dir ${AEN_NPU_GEN_DIR} --out ${AEN_NPU_MODEL_H}
		RESULT_VARIABLE _gen_rc
	)
	if(NOT _gen_rc EQUAL 0)
		message(FATAL_ERROR "aen-npu-inference: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_npu_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_npu_model)

target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})
target_sources(app PRIVATE src/main.cpp)
