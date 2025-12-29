cmake_minimum_required(VERSION 3.15)

project(alp_sdk 
    VERSION 0.1.0
    DESCRIPTION "ALP SDK - Unified Embedded HAL with VFT Pattern"
    LANGUAGES C
)

# C Standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Build options
option(BUILD_MOCK "Build with Mock driver (no vendor SDK required)" ON)
option(BUILD_ALIF "Build with Alif support (requires Alif SDK)" OFF)
option(BUILD_RENESAS "Build with Renesas support (requires Renesas FSP)" OFF)
option(BUILD_EXAMPLES "Build example applications" ON)
option(BUILD_TESTS "Build unit tests" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Vendor SDK paths
set(ALIF_SDK_PATH "" CACHE PATH "Path to Alif Ensemble SDK")
set(RENESAS_FSP_PATH "" CACHE PATH "Path to Renesas FSP")

# Output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Print configuration
message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "  ALP SDK Configuration")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "  Version:           ${PROJECT_VERSION}")
message(STATUS "  C Standard:        C${CMAKE_C_STANDARD}")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "  Build Options:")
message(STATUS "    Mock Driver:     ${BUILD_MOCK}")
message(STATUS "    Alif Driver:     ${BUILD_ALIF}")
message(STATUS "    Renesas Driver:  ${BUILD_RENESAS}")
message(STATUS "    Examples:        ${BUILD_EXAMPLES}")
message(STATUS "    Tests:           ${BUILD_TESTS}")
message(STATUS "    Shared libs:     ${BUILD_SHARED_LIBS}")
message(STATUS "")

if(BUILD_ALIF AND NOT ALIF_SDK_PATH)
    message(WARNING "BUILD_ALIF is ON but ALIF_SDK_PATH is not set!")
    message(WARNING "Please set: -DALIF_SDK_PATH=/path/to/alif/sdk")
endif()

if(BUILD_RENESAS AND NOT RENESAS_FSP_PATH)
    message(WARNING "BUILD_RENESAS is ON but RENESAS_FSP_PATH is not set!")
    message(WARNING "Please set: -DRENESAS_FSP_PATH=/path/to/renesas/fsp")
endif()

message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "")

# ============================================================================
# Core Library (always built - platform independent)
# ============================================================================

add_library(alp_sdk_core
    alp/src/core/alp_spi_core.c
    alp/src/core/alp_gpio_core.c
    alp/src/core/alp_usart_core.c
)

target_include_directories(alp_sdk_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/alp/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/alp/board>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:board>
)

target_compile_definitions(alp_sdk_core
    PUBLIC
        ALP_SDK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        ALP_SDK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        ALP_SDK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# ============================================================================
# Mock Driver Library (NO VENDOR SDK REQUIRED!)
# ============================================================================

if(BUILD_MOCK)
    message(STATUS "🎉 Building Mock Driver (NO VENDOR SDK REQUIRED!)")
    
    add_library(alp_sdk_mock
        alp/src/drivers/mock/alp_spi_mock.c
        alp/src/drivers/mock/alp_gpio_mock.c
        alp/src/drivers/mock/alp_usart_mock.c
        alp/board/board_mock.c
    )
    
    target_link_libraries(alp_sdk_mock
        PUBLIC
            alp_sdk_core
    )
    
    target_compile_definitions(alp_sdk_mock
        PUBLIC
            ALP_PLATFORM_MOCK=1
    )
    
    # Mock is the default if no other platform is selected
    if(NOT BUILD_ALIF AND NOT BUILD_RENESAS)
        add_library(alp_sdk ALIAS alp_sdk_mock)
        message(STATUS "  ✅ Mock driver set as default ALP SDK implementation")
    endif()
endif()

# ============================================================================
# Alif Driver Library (requires CMSIS Pack or Alif SDK)
# ============================================================================

if(BUILD_ALIF)
    # Option 1: Use CMSIS Pack (recommended)
    # Option 2: Use downloaded SDK files (via scripts)
    
    if(NOT ALIF_SDK_PATH)
        # Try to detect CMSIS pack location
        if(WIN32)
            set(CMSIS_PACK_ROOT "$ENV{LOCALAPPDATA}/Arm/Packs" CACHE PATH "CMSIS Pack root directory")
        else()
            set(CMSIS_PACK_ROOT "$ENV{HOME}/.arm/Packs" CACHE PATH "CMSIS Pack root directory")
        endif()
        
        set(ALIF_PACK_PATH "${CMSIS_PACK_ROOT}/AlifSemiconductor/Ensemble/2.0.4")
        set(ARM_CMSIS_PATH "${CMSIS_PACK_ROOT}/ARM/CMSIS/6.0.0")
        
        if(EXISTS ${ALIF_PACK_PATH})
            message(STATUS "🔧 Building Alif Driver (CMSIS Pack)")
            message(STATUS "  Pack: ${ALIF_PACK_PATH}")
            set(ALIF_SDK_PATH ${ALIF_PACK_PATH})
            set(USE_CMSIS_PACK ON)
        else()
            message(FATAL_ERROR 
                "ALIF_SDK_PATH is required when BUILD_ALIF=ON\n"
                "  Set -DALIF_SDK_PATH=/path/to/alif/sdk\n"
                "  Or install CMSIS pack to: ${ALIF_PACK_PATH}")
        endif()
    else()
        message(STATUS "🔧 Building Alif Driver")
        message(STATUS "  Path: ${ALIF_SDK_PATH}")
    endif()
    
    if(NOT EXISTS ${ALIF_SDK_PATH})
        message(FATAL_ERROR "ALIF_SDK_PATH does not exist: ${ALIF_SDK_PATH}")
    endif()
    
    # Create Alif driver library
    add_library(alp_sdk_alif
        alp/src/drivers/alif/alp_spi_alif.c
        alp/src/drivers/alif/alp_gpio_alif.c
        alp/src/drivers/alif/alp_usart_alif.c
        alp/board/board_alif_e7_devkit.c
    )
    
    # Include directories
    if(USE_CMSIS_PACK)
        # CMSIS Pack structure
        target_include_directories(alp_sdk_alif
            PRIVATE
                ${ARM_CMSIS_PATH}/CMSIS/Driver/Include
                ${ALIF_SDK_PATH}/Alif_CMSIS/Include
                ${ALIF_SDK_PATH}/Alif_CMSIS/Source
                ${ALIF_SDK_PATH}/drivers/include
                ${ALIF_SDK_PATH}/Device/core/common/include
        )
        
        # Add CMSIS driver source files (compile them directly)
        target_sources(alp_sdk_alif
            PRIVATE
                ${ALIF_SDK_PATH}/Alif_CMSIS/Source/Driver_SPI.c
                ${ALIF_SDK_PATH}/drivers/source/spi.c
        )
        
        # RTE defines for CMSIS pack
        target_compile_definitions(alp_sdk_alif
            PRIVATE
                RTE_Drivers_SPI
                RTE_SPI0
                _RTE_
        )
        
        message(STATUS "  ✅ Using CMSIS Pack: AlifSemiconductor::Ensemble@2.0.4")
    else()
        # Downloaded SDK structure (via scripts)
        target_include_directories(alp_sdk_alif
            PRIVATE
                ${ALIF_SDK_PATH}/CMSIS
                ${ALIF_SDK_PATH}/include
                ${ALIF_SDK_PATH}/drivers
        )
        
        message(STATUS "  ⚠️  Using downloaded SDK headers (limited functionality)")
        message(STATUS "      For full support, install CMSIS pack or use complete SDK")
    endif()
    
    target_link_libraries(alp_sdk_alif
        PUBLIC
            alp_sdk_core
    )
    
    target_compile_definitions(alp_sdk_alif
        PUBLIC
            ALP_PLATFORM_ALIF=1
    )
    
    # Alif becomes default if mock is not built
    if(NOT BUILD_MOCK)
        add_library(alp_sdk ALIAS alp_sdk_alif)
        message(STATUS "  ✅ Alif driver set as default ALP SDK implementation")
    endif()
endif()

# ============================================================================
# Renesas Driver Library (requires Renesas FSP)
# ============================================================================

if(BUILD_RENESAS)
    if(NOT RENESAS_FSP_PATH)
        message(FATAL_ERROR "RENESAS_FSP_PATH is required when BUILD_RENESAS=ON")
    endif()
    
    if(NOT EXISTS ${RENESAS_FSP_PATH})
        message(FATAL_ERROR "RENESAS_FSP_PATH does not exist: ${RENESAS_FSP_PATH}")
    endif()
    
    message(STATUS "🔧 Building Renesas Driver")
    message(STATUS "  Path: ${RENESAS_FSP_PATH}")
    
    add_library(alp_sdk_renesas
        alp/src/drivers/renesas/alp_spi_renesas.c
    )
    
    target_include_directories(alp_sdk_renesas
        PRIVATE
            ${RENESAS_FSP_PATH}/ra/fsp/inc
            ${RENESAS_FSP_PATH}/ra/fsp/inc/api
    )
    
    target_link_libraries(alp_sdk_renesas
        PUBLIC
            alp_sdk_core
    )
    
    target_compile_definitions(alp_sdk_renesas
        PUBLIC
            ALP_PLATFORM_RENESAS=1
    )
endif()

# ============================================================================
# Examples
# ============================================================================

if(BUILD_EXAMPLES)
    message(STATUS "📚 Building Examples")
    
    # Getting Started Example (uses Mock)
    if(BUILD_MOCK)
        add_executable(getting_started
            examples/getting_started/getting_started.c
        )
        
        target_link_libraries(getting_started
            PRIVATE
                alp_sdk_mock
        )
        
        message(STATUS "  ✅ getting_started (Mock)")
        
        # Blink Example (Mock)
        add_executable(blink_mock
            examples/blink/blink.c
        )
        
        target_link_libraries(blink_mock
            PRIVATE
                alp_sdk_mock
        )
        
        message(STATUS "  ✅ blink_mock (Mock)")
        
        # Hello World Example (Mock)
        add_executable(hello_world_mock
            examples/hello_world/hello_world.c
        )
        
        target_link_libraries(hello_world_mock
            PRIVATE
                alp_sdk_mock
        )
        
        message(STATUS "  ✅ hello_world_mock (Mock)")
    endif()
    
    # Alif Examples (requires Alif SDK)
    if(BUILD_ALIF)
        add_executable(alif_spi_example
            examples/alif_spi_example/alif_spi_example.c
        )
        
        target_link_libraries(alif_spi_example
            PRIVATE
                alp_sdk_alif
        )
        
        target_compile_definitions(alif_spi_example
            PRIVATE
                BUILD_ALIF=1
        )
        
        message(STATUS "  ✅ alif_spi_example (Alif)")
        
        # Blink Example (Alif)
        add_executable(blink_alif
            examples/blink/blink.c
        )
        
        target_link_libraries(blink_alif
            PRIVATE
                alp_sdk_alif
        )
        
        target_compile_definitions(blink_alif
            PRIVATE
                BUILD_ALIF=1
        )
        
        message(STATUS "  ✅ blink_alif (Alif)")
        
        # Hello World Example (Alif)
        add_executable(hello_world_alif
            examples/hello_world/hello_world.c
        )
        
        target_link_libraries(hello_world_alif
            PRIVATE
                alp_sdk_alif
        )
        
        target_compile_definitions(hello_world_alif
            PRIVATE
                BUILD_ALIF=1
        )
        
        message(STATUS "  ✅ hello_world_alif (Alif)")
    endif()
endif()

# ============================================================================
# Tests
# ============================================================================

if(BUILD_TESTS)
    enable_testing()
    message(STATUS "🧪 Building Tests")
    
    # Unit tests for Mock driver
    if(BUILD_MOCK)
        add_executable(test_spi_mock
            tests/mock/test_spi_mock.c
        )
        
        target_link_libraries(test_spi_mock
            PRIVATE
                alp_sdk_mock
        )
        
        add_test(NAME test_spi_mock COMMAND test_spi_mock)
        message(STATUS "  ✅ test_spi_mock")
    endif()
endif()

# ============================================================================
# Installation
# ============================================================================

# Install libraries
install(TARGETS alp_sdk_core
    EXPORT alp_sdk_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

if(BUILD_MOCK)
    install(TARGETS alp_sdk_mock
        EXPORT alp_sdk_targets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )
endif()

# Install headers
install(DIRECTORY alp/include/
    DESTINATION include/alp
    FILES_MATCHING PATTERN "*.h"
)

# Install CMake config files
install(EXPORT alp_sdk_targets
    FILE alp_sdk_targets.cmake
    NAMESPACE alp::
    DESTINATION lib/cmake/alp_sdk
)

# Print build summary
message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "  Configuration Complete!")
message(STATUS "")
message(STATUS "  Build commands:")
message(STATUS "    cmake --build ${CMAKE_BINARY_DIR}")
message(STATUS "    cmake --build ${CMAKE_BINARY_DIR} --target install")
message(STATUS "")
if(BUILD_EXAMPLES AND BUILD_MOCK)
    message(STATUS "  Run example:")
    message(STATUS "    ${CMAKE_BINARY_DIR}/bin/getting_started")
    message(STATUS "")
endif()
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "")
