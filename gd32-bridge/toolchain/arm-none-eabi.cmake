# SPDX-License-Identifier: Apache-2.0
#
# Toolchain file for the GD32G553MEY7TR (ARM Cortex-M33).
#
# Expects arm-none-eabi-{gcc,g++,objcopy,objdump,size,ar,ld} on PATH
# (use the official Arm GNU Toolchain release, 13.x or newer).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy CACHE INTERNAL "")
set(CMAKE_OBJDUMP      arm-none-eabi-objdump CACHE INTERNAL "")
set(CMAKE_SIZE         arm-none-eabi-size    CACHE INTERNAL "")
set(CMAKE_AR           arm-none-eabi-ar      CACHE INTERNAL "")

# We're building a freestanding firmware image, not testing the
# toolchain by running an executable on the host.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS TRUE)
