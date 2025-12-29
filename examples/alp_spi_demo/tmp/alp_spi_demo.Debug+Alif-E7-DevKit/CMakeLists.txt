cmake_minimum_required(VERSION 3.27)

# Roots
include("../roots.cmake")

set(CONTEXT alp_spi_demo.Debug+Alif-E7-DevKit)
set(TARGET ${CONTEXT})
set(OUT_DIR "${SOLUTION_ROOT}/out/alp_spi_demo/Alif-E7-DevKit/Debug")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(LD_SCRIPT "${SOLUTION_ROOT}/RTE/Device/AE722F80F55D5LS_M55_HE/linker_ac6_mram.sct")
set(LD_SCRIPT_PP ${LD_SCRIPT})

# Processor Options
set(CPU Cortex-M55)
set(FPU DP_FPU)
set(DSP DSP)
set(MVE FP_FVE)
set(BYTE_ORDER Little-endian)

# Toolchain config map
include("toolchain.cmake")

# Setup project
project(${CONTEXT} LANGUAGES C)

# Compilation database
add_custom_target(database
  COMMAND ${CMAKE_COMMAND} -E make_directory "${OUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json" "${OUT_DIR}"
)
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})

# Setup context
add_executable(${CONTEXT})
set_target_properties(${CONTEXT} PROPERTIES PREFIX "" SUFFIX ".axf" OUTPUT_NAME "alp_spi_demo")
set_target_properties(${CONTEXT} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR})
add_library(${CONTEXT}_GLOBAL INTERFACE)

# Includes
target_include_directories(${CONTEXT} PUBLIC
  "${SOLUTION_ROOT}/RTE/_Debug_Alif-E7-DevKit"
  "${SOLUTION_ROOT}/RTE/Device/AE722F80F55D5LS_M55_HE"
  "${CMSIS_PACK_ROOT}/ARM/CMSIS/6.0.0/CMSIS/Core/Include"
  "${CMSIS_PACK_ROOT}/ARM/CMSIS/6.0.0/CMSIS/Driver/Include"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Alif_CMSIS/Include"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Alif_CMSIS/Source"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Device/core/common/include"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Device/soc/AE722F80F55D5/include"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Device/soc/AE722F80F55D5/include/rtss_he"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/Device/system/include"
  "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4/drivers/include"
)

# Defines
target_compile_definitions(${CONTEXT} PUBLIC
  $<$<COMPILE_LANGUAGE:C,CXX>:
    AE722F80F55D5
    RTSS_HE
    _RTE_
  >
)

# Compile options
target_compile_options(${CONTEXT} PUBLIC
  $<$<COMPILE_LANGUAGE:C>:
    "SHELL:${CC_CPU}"
    "SHELL:${CC_FLAGS}"
    "SHELL:${CC_BYTE_ORDER}"
    "SHELL:-xc"
    "SHELL:-std=gnu11"
    "SHELL:-I${SOLUTION_ROOT}/../../alp/include"
    "SHELL:-I${SOLUTION_ROOT}/../../alp/board"
    "SHELL:-DBUILD_ALIF=1"
  >
)

# Compile Options Abstractions
add_library(${CONTEXT}_ABSTRACTIONS INTERFACE)
cbuild_set_options_flags(CC "none" "on" "" "" CC_OPTIONS_FLAGS_${CONTEXT})
target_compile_options(${CONTEXT}_ABSTRACTIONS INTERFACE
  $<$<COMPILE_LANGUAGE:C>:
    "SHELL:${CC_OPTIONS_FLAGS_${CONTEXT}}"
  >
)

# Add groups and components
include("groups.cmake")
include("components.cmake")

target_link_libraries(${CONTEXT} PUBLIC
  Group_ALP_SDK_Core
  Group_ALP_SDK_Alif_Drivers
  Group_Board_Support
  Group_Application
  ARM_CMSIS_CORE_6_0_0
  AlifSemiconductor_CMSIS_Driver_SPI_2_0_0
  AlifSemiconductor_CMSIS_Driver_USART_2_0_0
  AlifSemiconductor_Device_SOC_Peripherals_GPIO_2_0_0
  AlifSemiconductor_Device_SOC_Peripherals_PINCONF_2_0_0
  AlifSemiconductor_Device_Startup_2_0_0
)

# Linker options
target_link_options(${CONTEXT} PUBLIC
  "SHELL:${LD_CPU}"
  "SHELL:${_LS}\"${LD_SCRIPT_PP}\""
)
set_target_properties(${CONTEXT} PROPERTIES LINK_DEPENDS ${LD_SCRIPT})
