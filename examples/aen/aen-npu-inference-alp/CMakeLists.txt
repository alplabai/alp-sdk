# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-inference-alp -- the real person_detect MobileNet on the E1M-AEN801
# (Ensemble E8) Ethos-U85, driven through the portable <alp/inference.h> API
# (alp_inference_open/invoke/get_output).  Unlike the sibling
# aen-npu-inference-person-mram (raw Arm InferenceProcess + in-app strong
# overrides), the app here writes ONLY the portable API: the AEN Ethos-U backend
# (src/backends/inference/ethos_u_aen.cpp) supplies the NPU AXI address-remap +
# SRAM-port config-select overrides.  ~263 KiB Vela model -> MRAM slot0, Flow D.
#
# LICENSING: a bench-matching Vela command stream needs Alif's PROPRIETARY
# ensemble_vela.ini (in alp-sdk-internal, NOT redistributed by alp-sdk); pass it
# with -DAEN_NPU_VELA_CONFIG=<internal>/ensemble_vela.ini.  Without it Vela uses
# its built-in system-config (a build-only check; the emitted command stream
# will NOT match the bench-validated E8 runtime).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_inference_alp LANGUAGES C CXX)

# --------------------------------------------------------------------------
# Model-gen: Vela-compile person_detect for the U85 (same as the raw sibling).
# --------------------------------------------------------------------------
set(AEN_NPU_VELA   "vela"
    CACHE FILEPATH "Vela compiler (on PATH, or override: -DAEN_NPU_VELA=<venv>/bin/vela)")
set(AEN_NPU_ACCEL  "ethos-u85-256"
    CACHE STRING   "Vela --accelerator-config (the AEN801 U85 is 256-MAC)")
set(AEN_NPU_MODEL  "${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro/models/person_detect.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "person_detect_u85"
    CACHE STRING   "Embedded model identity string")
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
if(NOT AEN_NPU_VELA_CONFIG)
	message(WARNING
		"aen-npu-inference-alp: AEN_NPU_VELA_CONFIG unset -- Vela will use its "
		"built-in system-config and the emitted command stream will NOT match the "
		"bench-validated E8 runtime.  Pass -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/ensemble_vela.ini.")
endif()
set(AEN_NPU_SYSTEM_CONFIG "Ethos_U85_SRAM_Only" CACHE STRING "Vela --system-config")
set(AEN_NPU_MEMORY_MODE   "Sram_Only"           CACHE STRING "Vela --memory-mode")

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
	COMMENT "aen-npu-inference-alp: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL}"
	VERBATIM
)
if(NOT EXISTS ${AEN_NPU_MODEL_H})
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
		COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
		RESULT_VARIABLE _gen_rc)
	if(NOT _gen_rc EQUAL 0)
		message(FATAL_ERROR "aen-npu-inference-alp: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_npu_alp_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_npu_alp_model)

# Publish hal_alif's common/include on the global Zephyr include path so the AEN
# Ethos-U backend's strong ethosu_address_remap (src/backends/inference/
# ethos_u_aen.cpp) can #include <soc_memory_map.h> (local_to_global()).  Header
# only -- not CONFIG_RTSS_HE, which would also compile the TZ sources that
# hard-#error without a non-secure DTS node on this bench board.
zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)

target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})
target_sources(app PRIVATE src/main.c)
