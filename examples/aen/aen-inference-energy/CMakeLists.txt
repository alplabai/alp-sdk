# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-inference-energy -- on-target inference ENERGY measurement for the
# E1M-AEN801 (Ensemble E8, M55-HE).  Runs a Vela-compiled int8 model N times
# while sampling an on-EVK INA236 rail monitor, and reports the incremental
# millijoules per inference.
#
# The NPU build wiring is deliberately IDENTICAL to the silicon-proven
# examples/aen/aen-npu-inference-alif, and reuses that app's gen_model.py
# rather than copying it: the Vela invocation (accelerator config, Alif
# system-config, the SRAM0 placement macros it emits) is one hardware fact and
# must not fork into two drifting copies.  Only the model-gen script is shared;
# every config file here is local.
#
# Modules required at build time (pass via -DEXTRA_ZEPHYR_MODULES):
#   the alp-sdk checkout    (the SDK + ARM_ETHOS_U glue + chips/ina236)
#   hal_alif                (soc_memory_map.h / local_to_global)
#   hal/ethos_u             (the Arm Ethos-U core driver lib)
#   lib/tflite-micro        (TFLM + the Ethos-U custom op)
# scripts/bench/aen/build.sh passes the first two automatically.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_inference_energy LANGUAGES C CXX)

set(AEN_NPU_INFERENCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../aen-npu-inference-alif"
    CACHE PATH "Sibling example whose gen_model.py (the Vela driver) is SHARED, not copied")

# Fail with the actual reason.  Copying this example out of the SDK tree -- the
# normal customer path -- leaves the sibling behind, and without this check the
# build dies as an opaque "model-gen failed (rc=1)" from the custom command.
if(NOT EXISTS "${AEN_NPU_INFERENCE_DIR}/gen_model.py")
	message(FATAL_ERROR
		"aen-inference-energy: cannot find ${AEN_NPU_INFERENCE_DIR}/gen_model.py.\n"
		"This example deliberately SHARES the Vela driver script with "
		"examples/aen/aen-npu-inference-alif rather than copying it, so the two "
		"cannot drift.  Keep both example directories together, or point "
		"-DAEN_NPU_INFERENCE_DIR=<path to aen-npu-inference-alif> at it.")
endif()

# --------------------------------------------------------------------------
# Model-gen: Vela-compile the fixture for the U85 with the Alif system-config.
# --------------------------------------------------------------------------
set(AEN_NPU_VELA   "vela"
    CACHE FILEPATH "Vela compiler executable (on PATH, or override: -DAEN_NPU_VELA=<venv>/bin/vela)")
set(AEN_NPU_ACCEL  "ethos-u85-256"
    CACHE STRING   "Vela --accelerator-config (the AEN801 U85 is 256-MAC)")
set(AEN_NPU_MODEL  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fixtures/models/tiny_int8.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "tiny_int8_fixture"
    CACHE STRING   "Embedded model identity string")

# ALIF PROPRIETARY Vela config -- NOT redistributed by alp-sdk.  Obtain it from
# alp-sdk-internal and pass -DAEN_NPU_VELA_CONFIG=<path>/ensemble_vela.ini.
# When unset, Vela uses its built-in system-config; the app still runs on the
# NPU (src/main.cpp pins every region to the SRAM AXI port, which is what the
# SRAM_Only layout needs), but the command stream is not the bench-matched one.
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Path to the Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
set(AEN_NPU_SYSTEM_CONFIG "Ethos_U85_SRAM_Only"
    CACHE STRING   "Vela --system-config (HE core, single SRAM region)")
set(AEN_NPU_MEMORY_MODE   "Sram_Only"
    CACHE STRING   "Vela --memory-mode (single region -> SRAM0)")

set(AEN_NPU_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/model-gen")
set(AEN_NPU_MODEL_H "${AEN_NPU_GEN_DIR}/model.h")

set(_gen_args
	--vela          ${AEN_NPU_VELA}
	--accel         ${AEN_NPU_ACCEL}
	--model         ${AEN_NPU_MODEL}
	--name          ${AEN_NPU_MODEL_NAME}
	--build-dir     ${AEN_NPU_GEN_DIR}
	--out           ${AEN_NPU_MODEL_H}
)
# --system-config/--memory-mode name sections that live ONLY in the Alif
# proprietary ensemble_vela.ini, so pass them ONLY when that config is supplied
# -- else Vela errors "Section ... not found" against its built-in vela.ini.
if(AEN_NPU_VELA_CONFIG)
	list(APPEND _gen_args --config ${AEN_NPU_VELA_CONFIG}
	     --system-config ${AEN_NPU_SYSTEM_CONFIG} --memory-mode ${AEN_NPU_MEMORY_MODE})
endif()

add_custom_command(
	OUTPUT  ${AEN_NPU_MODEL_H}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
	COMMAND ${PYTHON_EXECUTABLE} ${AEN_NPU_INFERENCE_DIR}/gen_model.py ${_gen_args}
	DEPENDS ${AEN_NPU_MODEL} ${AEN_NPU_INFERENCE_DIR}/gen_model.py
	COMMENT "aen-inference-energy: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL}"
	VERBATIM
)

# Configure-time generation (so the include path resolves on the first build).
if(NOT EXISTS ${AEN_NPU_MODEL_H})
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
		COMMAND ${PYTHON_EXECUTABLE} ${AEN_NPU_INFERENCE_DIR}/gen_model.py ${_gen_args}
		RESULT_VARIABLE _gen_rc
	)
	if(NOT _gen_rc EQUAL 0)
		message(FATAL_ERROR "aen-inference-energy: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_energy_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_energy_model)

# Publish hal_alif's common/include so <soc_memory_map.h> (local_to_global())
# resolves.  hal_alif adds it only under CONFIG_RTSS_HE/HP, but that same gate
# also compiles TrustZone sources that hard-#error without a non-secure DTS
# node this RAM-run board does not have -- so publish just the headers.
zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)

target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})
target_sources(app PRIVATE src/main.cpp)

# --------------------------------------------------------------------------
# Measurement knobs -> preprocessor defines.
#
# A `-DFOO=bar` on the west/cmake command line only sets a CMake CACHE
# variable; it does NOT reach the compiler.  Without this forwarding the
# overrides below are accepted silently and have no effect -- the build even
# produces a byte-identical image, so the run looks like it honoured them and
# quietly measures something else.  Each is left EMPTY by default so src/main.cpp
# keeps its own documented #ifndef default.
# --------------------------------------------------------------------------
set(AEN_ENERGY_SAMPLES_PER_WINDOW "" CACHE STRING
    "INA236 conversions to capture per window (default 250 in src/main.cpp)")
set(AEN_ENERGY_WINDOW_PAIRS "" CACHE STRING
    "Repeated (active, idle) window pairs (default 3)")
set(AEN_ENERGY_EMIT_SAMPLES "" CACHE STRING
    "1 = print every sample for host re-integration; 0 = summary only (long windows)")
set(AEN_ENERGY_RAIL_ADDR "" CACHE STRING
    "Pin the rail monitor by 7-bit I2C address (e.g. 0x4A); default is auto-select")

foreach(_knob AEN_ENERGY_SAMPLES_PER_WINDOW AEN_ENERGY_WINDOW_PAIRS
              AEN_ENERGY_EMIT_SAMPLES AEN_ENERGY_RAIL_ADDR)
	if(NOT "${${_knob}}" STREQUAL "")
		target_compile_definitions(app PRIVATE ${_knob}=${${_knob}})
		message(STATUS "aen-inference-energy: ${_knob}=${${_knob}}")
	endif()
endforeach()
