# SPDX-License-Identifier: Apache-2.0
#
# Toolchain file for the CC3501E application core (ARM Cortex-M33),
# used for the SILICON-FREE stub backend -- the CI compile smoke and
# the host unit test's cross-compile path.  The production image is
# built with TI ticlang (toolchain/ticlang.cmake); the stub layer is
# toolchain-agnostic portable C, so the freely-available Arm GNU
# Toolchain gives a fast, SDK-free "does it compile for the target
# arch?" gate.
#
# Expects arm-none-eabi-{gcc,objcopy,objdump,size,ar} on PATH (Arm GNU
# Toolchain, 10.3 or newer).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy CACHE INTERNAL "")
set(CMAKE_OBJDUMP      arm-none-eabi-objdump CACHE INTERNAL "")
set(CMAKE_SIZE         arm-none-eabi-size    CACHE INTERNAL "")
set(CMAKE_AR           arm-none-eabi-ar      CACHE INTERNAL "")

# Freestanding firmware image; don't try to run a probe executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS TRUE)
