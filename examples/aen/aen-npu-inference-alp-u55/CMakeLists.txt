# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-inference-alp-u55 -- real person_detect on the E1M-AEN801 (Ensemble
# E8) Ethos-U55 (the M55-HE-paired U55, 128-MAC), driven through the portable
# <alp/inference.h> API.  The U55 sibling of aen-npu-inference-alp (which runs
# the U85).  The app source + gen_model.py + overlay/prj.conf differ only in the
# accelerator config; the alp_inference_open() code is identical -- the AEN
# Ethos-U backend supplies the (variant-agnostic) strong overrides.
#
# LICENSING: a bench-matching Vela stream needs Alif's PROPRIETARY
# ensemble_vela.ini (-DAEN_NPU_VELA_CONFIG=..., not redistributed).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_inference_alp_u55 LANGUAGES C CXX)

set(AEN_NPU_VELA   "vela"
    CACHE FILEPATH "Vela compiler (on PATH, or -DAEN_NPU_VELA=<venv>/bin/vela)")
# THE DIFFERENCE vs the U85 sibling: the E8 per-core-local NPU is a U55, and
# its MAC count is core-specific -- the M55-HE-paired U55 is 128-MAC, the
# M55-HP-paired U55 (same shared DT node, different silicon) is 256-MAC (see
# ensemble_e8_peripherals.dtsi).  Pick the Vela accelerator config + Alif
# system-config off BOARD so a single app source builds correctly for
# whichever core's board target is passed to `west build -b`.
if(BOARD MATCHES "_m55_hp$")
	set(_aen_npu_accel_default      "ethos-u55-256")
	set(_aen_npu_sysconfig_default  "RTSS_HP_SRAM_Only")
else()
	set(_aen_npu_accel_default      "ethos-u55-128")
	set(_aen_npu_sysconfig_default  "RTSS_HE_SRAM_Only")
endif()
set(AEN_NPU_ACCEL  "${_aen_npu_accel_default}"
    CACHE STRING   "Vela --accelerator-config (128-MAC on M55-HE, 256-MAC on M55-HP)")
set(AEN_NPU_MODEL  "${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro/models/person_detect.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "person_detect_u55"
    CACHE STRING   "Embedded model identity string")
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
if(NOT AEN_NPU_VELA_CONFIG)
	message(WARNING
		"aen-npu-inference-alp-u55: AEN_NPU_VELA_CONFIG unset -- Vela will use its "
		"built-in system-config and the command stream will NOT match the bench "
		"runtime.  Pass -DAEN_NPU_VELA_CONFIG=<alif>/ensemble_vela.ini.")
endif()
# Each core's U55 uses that core's SRAM-only memory view (the Alif
# ensemble_vela.ini has no Ethos_U55-specific section; the per-core
# RTSS_HE/RTSS_HP config describes the U55's memory: axi0_port=Sram).
set(AEN_NPU_SYSTEM_CONFIG "${_aen_npu_sysconfig_default}" CACHE STRING "Vela --system-config")
set(AEN_NPU_MEMORY_MODE   "Sram_Only"         CACHE STRING "Vela --memory-mode")

set(AEN_NPU_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/model-gen")
set(AEN_NPU_MODEL_H "${AEN_NPU_GEN_DIR}/model.h")

set(_gen_args
	--vela      ${AEN_NPU_VELA}
	--accel     ${AEN_NPU_ACCEL}
	--model     ${AEN_NPU_MODEL}
	--name      ${AEN_NPU_MODEL_NAME}
	--build-dir ${AEN_NPU_GEN_DIR}
	--out       ${AEN_NPU_MODEL_H}
)
if(AEN_NPU_VELA_CONFIG)
	list(APPEND _gen_args --config ${AEN_NPU_VELA_CONFIG}
	     --system-config ${AEN_NPU_SYSTEM_CONFIG} --memory-mode ${AEN_NPU_MEMORY_MODE})
endif()

add_custom_command(
	OUTPUT  ${AEN_NPU_MODEL_H}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
	DEPENDS ${AEN_NPU_MODEL} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
	COMMENT "aen-npu-inference-alp-u55: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL}"
	VERBATIM
)
if(NOT EXISTS ${AEN_NPU_MODEL_H})
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
		COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
		RESULT_VARIABLE _gen_rc)
	if(NOT _gen_rc EQUAL 0)
		message(FATAL_ERROR "aen-npu-inference-alp-u55: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_npu_alp_u55_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_npu_alp_u55_model)

zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)
target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})
target_sources(app PRIVATE src/main.c)
