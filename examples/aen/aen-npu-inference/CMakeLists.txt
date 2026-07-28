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
#   <alp-sdk>                                   (the SDK; provides ALP_SDK Kconfig)
#   <zephyr-workspace>/modules/hal/alif         (hal_alif -- SoC support)
#   <zephyr-workspace>/modules/hal/ethos_u      (Arm Ethos-U core driver lib)
#   <tflite-micro path>                         (TFLM sources + Zephyr glue)
#
# The model-gen step (gen_model.py) runs vela from .venv-vela and writes
# model.h into the build tree, so the source tree carries no generated blob.
#
# LICENSING (alp-sdk doctrine): the Alif Vela system-config (ensemble_vela.ini)
# is ALIF PROPRIETARY and is NOT vendored / redistributed by alp-sdk.  It is
# injected at build time from alp-sdk-internal via -DAEN_NPU_VELA_CONFIG=<path>
# (default empty -> Vela's built-in system-config, which does NOT match the
# bench-validated E8 RTSS-HE runtime).
#
# The ethosu_invoke=1 fix itself is alp-sdk's AEN Ethos-U backend
# (src/backends/inference/ethos_u_aen.cpp, CONFIG_ALP_SDK_INFERENCE_BACKEND_
# ETHOS_U_AEN=y in prj.conf) -- it supplies the strong ethosu_address_remap /
# ethosu_config_select overrides.  Measured 2026-07-28: the Vela
# --memory-mode/--system-config flags are NOT that fix by themselves (a
# byte-identical Vela blob to the aen-npu-inference-alif sibling still
# returned invoke=1 without the overrides); they only make the compiled
# command stream match the bench-validated Alif system-config when
# AEN_NPU_VELA_CONFIG is supplied.

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
#
# Vela default: prefer `vela` on PATH (a venv the user has activated, a
# system install, ...); fall back to the in-tree .venv-vela only if PATH has
# nothing.  That fallback assumes a venv location relative to the SOURCE
# tree, which does not exist in every checkout (e.g. a git worktree) -- so
# it is a fallback, not the primary lookup.  -DAEN_NPU_VELA=<path> always
# wins over both (CACHE set here is a no-op once the var is already defined
# on the command line).
find_program(_aen_npu_vela_on_path vela)
if(_aen_npu_vela_on_path)
    set(_aen_npu_vela_default ${_aen_npu_vela_on_path})
else()
    set(_aen_npu_vela_default "${CMAKE_CURRENT_SOURCE_DIR}/../../../.venv-vela/bin/vela")
endif()
set(AEN_NPU_VELA   "${_aen_npu_vela_default}"
    CACHE FILEPATH "Path to the Vela compiler executable (defaults to `vela` on PATH, else the in-tree .venv-vela)")
set(AEN_NPU_ACCEL  "ethos-u85-256"
    CACHE STRING   "Vela --accelerator-config (the AEN801 U85 is 256-MAC)")
set(AEN_NPU_MODEL  "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/fixtures/models/tiny_int8.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "tiny_int8_fixture"
    CACHE STRING   "Embedded model identity string")

# ALIF PROPRIETARY Vela config -- NOT redistributed by alp-sdk.  Obtain it from
# alp-sdk-internal and pass it via -DAEN_NPU_VELA_CONFIG=<path>/ensemble_vela.ini.
# When unset, Vela uses its built-in system-config (does NOT match the bench).
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Path to the Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
if(NOT AEN_NPU_VELA_CONFIG)
	message(WARNING
		"aen-npu-inference: AEN_NPU_VELA_CONFIG unset -- Vela will use its built-in "
		"system-config and the emitted command stream will NOT match the "
		"bench-validated E8 runtime.  Pass -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/ensemble_vela.ini.")
endif()
set(AEN_NPU_SYSTEM_CONFIG "Ethos_U85_SRAM_Only"
    CACHE STRING   "Vela --system-config (HE core, single SRAM region)")
set(AEN_NPU_MEMORY_MODE   "Sram_Only"
    CACHE STRING   "Vela --memory-mode (single region -> SRAM0)")

set(AEN_NPU_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/model-gen")
set(AEN_NPU_MODEL_H "${AEN_NPU_GEN_DIR}/model.h")

# Common gen_model.py args (the Vela --config is appended only when the
# Alif-proprietary .ini is supplied, so an unset config omits the flag).
set(_gen_args
	--vela       ${AEN_NPU_VELA}
	--accel      ${AEN_NPU_ACCEL}
	--model      ${AEN_NPU_MODEL}
	--name       ${AEN_NPU_MODEL_NAME}
	--build-dir  ${AEN_NPU_GEN_DIR}
	--out        ${AEN_NPU_MODEL_H}
)
# --system-config/--memory-mode name sections that live ONLY in the Alif
# proprietary ensemble_vela.ini -- pass them ONLY when that config is
# supplied, else Vela errors "Section ... not found" against its built-in
# vela.ini (matches the aen-npu-inference-alif sibling's pattern).
if(AEN_NPU_VELA_CONFIG)
	list(APPEND _gen_args --config ${AEN_NPU_VELA_CONFIG}
	     --system-config ${AEN_NPU_SYSTEM_CONFIG} --memory-mode ${AEN_NPU_MEMORY_MODE})
endif()

# Run at configure time so the header exists before the first compile, and
# again as a build rule so editing the input model / gen script re-runs it.
add_custom_command(
	OUTPUT  ${AEN_NPU_MODEL_H}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
	DEPENDS ${AEN_NPU_MODEL} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
	COMMENT "aen-npu-inference: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL}"
	VERBATIM
)

# Configure-time generation (so the include path resolves on the first build).
if(NOT EXISTS ${AEN_NPU_MODEL_H})
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
		COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
		RESULT_VARIABLE _gen_rc
	)
	if(NOT _gen_rc EQUAL 0)
		message(FATAL_ERROR "aen-npu-inference: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_npu_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_npu_model)

# Publish hal_alif's common/include on the GLOBAL Zephyr include path so
# alp-sdk's AEN Ethos-U backend (src/backends/inference/ethos_u_aen.cpp,
# enabled by CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN in prj.conf) can
# #include <soc_memory_map.h> (local_to_global(), its strong
# ethosu_address_remap uses).  Header only -- not CONFIG_RTSS_HE, which would
# also compile the TZ sources that hard-#error without a non-secure DTS node
# on this bench board (see aen-npu-inference-alif's CMakeLists.txt).
zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)

target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})
target_sources(app PRIVATE src/main.cpp)
