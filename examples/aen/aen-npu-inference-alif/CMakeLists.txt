# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# aen-npu-inference-alif -- the MATCHED-RUNTIME NPU inference app for the
# E1M-AEN801 (Ensemble E8, M55-HE).  A Vela-compiled int8 model runs through
# the Arm Apache-2.0 InferenceProcess wrapper (lib/ethosu_utils, the same
# runtime Alif's tflm_ethosu / executorch samples use) -> TFLM MicroInterpreter
# with AddEthosU() -> ethosu_invoke onto the Ethos-U85 (256-MAC).
#
# DIFFERENCE vs the sibling aen-npu-inference (which returned ethosu_invoke=1):
#   1. CONFIG_ARM_ETHOS_U=y (prj.conf) -> hal_alif compiles ethosu_callback.c,
#      whose STRONG ethosu_address_remap = local_to_global overrides the Arm
#      core driver's weak identity remap.  (alp-sdk defines CONFIG_ARM_ETHOS_U
#      in zephyr/Kconfig + `select`s the upstream ETHOS_U driver.)
#   2. Vela is run with the ALIF system-config (--config <ensemble_vela.ini>
#      --system-config Ethos_U85_SRAM_Only --memory-mode Sram_Only) so the
#      command stream matches the E8 RTSS-HE runtime.
#   3. The overlay re-`compatible`s the itcm/dtcm nodes onto alif,itcm/alif,dtcm
#      and adds the global_base props local_to_global() needs.
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
project(aen_npu_inference_alif LANGUAGES C CXX)

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
# alp-sdk-internal and pass it via -DAEN_NPU_VELA_CONFIG=<path>/ensemble_vela.ini.
# When unset, Vela uses its built-in system-config (does NOT match the bench).
set(AEN_NPU_VELA_CONFIG ""
    CACHE FILEPATH "Path to the Alif Vela config .ini (ALIF PROPRIETARY -- from alp-sdk-internal)")
if(NOT AEN_NPU_VELA_CONFIG)
	message(WARNING
		"aen-npu-inference-alif: AEN_NPU_VELA_CONFIG unset -- Vela will use its "
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
	--build-dir     ${AEN_NPU_GEN_DIR}
	--out           ${AEN_NPU_MODEL_H}
)
# --system-config/--memory-mode name sections that live ONLY in the Alif
# proprietary ensemble_vela.ini (e.g. Ethos_U85_SRAM_Only) -- pass them ONLY
# when that config is supplied, else Vela errors "Section ... not found" against
# its built-in vela.ini.  Without the Alif config Vela falls back to its default
# system-config (the build succeeds; the command stream just won't match bench).
if(AEN_NPU_VELA_CONFIG)
	list(APPEND _gen_args --config ${AEN_NPU_VELA_CONFIG}
	     --system-config ${AEN_NPU_SYSTEM_CONFIG} --memory-mode ${AEN_NPU_MEMORY_MODE})
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
