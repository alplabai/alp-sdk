# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-inference-person-mram -- a REAL-MODEL, MRAM-RESIDENT NPU inference app
# for the E1M-AEN801 (Ensemble E8, M55-HE).  Runs the person_detect MobileNet
# (int8) through the Arm Apache-2.0 InferenceProcess wrapper (ethosu_utils, the
# same runtime Alif's tflm_ethosu / executorch samples use) -> TFLM
# MicroInterpreter with AddEthosU() -> ethosu_invoke onto the Ethos-U85 (256-MAC).
#
# DIFFERENCE vs the sibling aen-npu-inference-alif (the hermetic tiny_int8
# fixture, an ITCM RAM-run image): person_detect Vela'd for the U85 is ~263 KiB
# and OVERFLOWS the 256 KiB ITCM RAM-run region, so this variant DROPS the &itcm
# overlay retarget and links into the MRAM slot0 boot partition (@0x80010000) --
# flashed + booted via Flow D (J-Link direct MRAM).  The matched-runtime fix is
# identical (strong in-app ethosu_address_remap + ethosu_config_select; see
# src/main.cpp), and the model rodata still lives in flash (now MRAM) and is
# memcpy'd into NPU-reachable SRAM0 at boot.
#
# Modules required at build time (pass via -DEXTRA_ZEPHYR_MODULES):
#   the alp-sdk checkout    (the SDK + ARM_ETHOS_U glue)
#   hal_alif                (the Alif HAL module -- ethosu_callback.c)
#   hal/ethos_u             (the Arm Ethos-U core driver lib)
#   lib/tflite-micro        (TFLM + the Ethos-U custom op)
#
# LICENSING (alp-sdk doctrine):
#   - lib/ethosu_utils/{inference_process.cpp,.hpp} + include/ethosu/InferenceRunner.h
#     are Apache-2.0 (Arm) -> VENDORED into this example (Tier-2, provenance hdr).
#   - The Alif Vela config (ensemble_vela.ini) + the subsys/modules/ethosu glue
#     are ALIF PROPRIETARY -> NOT vendored and NOT redistributed by alp-sdk.
#     Obtain ensemble_vela.ini from alp-sdk-internal and pass it explicitly:
#       -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/.../ensemble_vela.ini
#     Without it Vela falls back to its built-in system-config and the emitted
#     command stream will NOT match the bench-validated E8 RTSS-HE runtime.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(aen_npu_inference_person_mram LANGUAGES C CXX)

# --------------------------------------------------------------------------
# Model-gen: Vela-compile person_detect for the U85 with the Alif system-config.
# --------------------------------------------------------------------------
set(AEN_NPU_VELA   "vela"
    CACHE FILEPATH "Vela compiler executable (on PATH, or override: -DAEN_NPU_VELA=<venv>/bin/vela)")
set(AEN_NPU_ACCEL  "ethos-u85-256"
    CACHE STRING   "Vela --accelerator-config (the AEN801 U85 is 256-MAC)")
# Default: the person_detect int8 MobileNet from the tflite-micro module (resolved
# via the Zephyr module dir, no local path).  Override with -DAEN_NPU_MODEL=<.tflite>.
set(AEN_NPU_MODEL  "${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/tensorflow/lite/micro/models/person_detect.tflite"
    CACHE FILEPATH "Input int8 .tflite to Vela-compile")
set(AEN_NPU_MODEL_NAME "person_detect_u85"
    CACHE STRING   "Embedded model identity string")

# Input-tensor byte count the InferenceProcess job advertises -- MUST equal the
# model's input-tensor size or runJob aborts with "Input tensor size mismatch".
# person_detect [1,96,96,1] int8 = 9216; keyword_scrambled_8bit [1,96] int16 =
# 192.  Override with -DAEN_NPU_INPUT_BYTES=<n> when swapping a model.
set(AEN_NPU_INPUT_BYTES "9216"
    CACHE STRING   "Model input-tensor size in bytes (per-model)")

# ALIF PROPRIETARY Vela config -- NOT redistributed by alp-sdk.  Obtain it from
# alp-sdk-internal and pass it via -DAEN_NPU_VELA_CONFIG=<path>/ensemble_vela.ini.
# When unset, Vela uses its built-in system-config (does NOT match the bench).
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Path to the Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
if(NOT AEN_NPU_VELA_CONFIG)
	message(WARNING
		"aen-npu-inference-person-mram: AEN_NPU_VELA_CONFIG unset -- Vela will use its "
		"built-in system-config and the emitted command stream will NOT match the "
		"bench-validated E8 runtime.  Pass -DAEN_NPU_VELA_CONFIG=<alp-sdk-internal>/ensemble_vela.ini.")
endif()
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
	--system-config ${AEN_NPU_SYSTEM_CONFIG}
	--memory-mode   ${AEN_NPU_MEMORY_MODE}
	--build-dir     ${AEN_NPU_GEN_DIR}
	--out           ${AEN_NPU_MODEL_H}
)
if(AEN_NPU_VELA_CONFIG)
	list(APPEND _gen_args --config ${AEN_NPU_VELA_CONFIG})
endif()

add_custom_command(
	OUTPUT  ${AEN_NPU_MODEL_H}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${AEN_NPU_GEN_DIR}
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py ${_gen_args}
	DEPENDS ${AEN_NPU_MODEL} ${CMAKE_CURRENT_SOURCE_DIR}/gen_model.py
	COMMENT "aen-npu-inference-alif: Vela-compiling ${AEN_NPU_MODEL} for ${AEN_NPU_ACCEL} (${AEN_NPU_SYSTEM_CONFIG})"
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
		message(FATAL_ERROR "aen-npu-inference-alif: model-gen failed (rc=${_gen_rc})")
	endif()
endif()

add_custom_target(aen_npu_model DEPENDS ${AEN_NPU_MODEL_H})
add_dependencies(app aen_npu_model)

# --------------------------------------------------------------------------
# Publish hal_alif's common/include on the GLOBAL Zephyr include path so the
# hal_alif Ethos-U callback library (modules/hal/alif/drivers/ethos_u/src/
# ethosu_callback.c) can #include <soc_memory_map.h> (the local_to_global() the
# strong ethosu_address_remap uses).
#
# WHY HERE and not via CONFIG_RTSS_HE: hal_alif's common/CMakeLists.txt adds
# that include dir only under `if(CONFIG_RTSS_HE OR CONFIG_RTSS_HP)`, but that
# same gate also compiles the TrustZone sources sau_tcm_ns_setup.c / tgu.c,
# which hard-#error without a non-secure `ns` DTS node (the fork has one; this
# RAM-run board does not, and inventing one would be inventing HW config).  We
# therefore publish ONLY the header directory and leave the TZ sources off.
# zephyr_include_directories() adds to the global interface, so the header is
# visible to the hal_alif ethos_u library TU as well as the app.
zephyr_include_directories(${ZEPHYR_ALIF_MODULE_DIR}/common/include)

target_include_directories(app PRIVATE ${AEN_NPU_GEN_DIR})

# Apache-2.0 Arm InferenceProcess wrapper (vendored into this example).
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ethosu_utils)
target_sources(app PRIVATE
	src/main.cpp
	ethosu_utils/inference_process.cpp
)

# Make the per-model input size a real compile define so -DAEN_NPU_INPUT_BYTES
# reaches main.cpp's input_sram[] + the job's input DataPtr size.
target_compile_definitions(app PRIVATE AEN_NPU_INPUT_BYTES=${AEN_NPU_INPUT_BYTES})
