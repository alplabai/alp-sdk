# SPDX-License-Identifier: Apache-2.0
#
# Toolchain file for the PRODUCTION cc3501e-bridge image: TI's ticlang
# (tiarmclang -- TI's Arm LLVM compiler), the toolchain TI's SimpleLink
# CC33xx SDK is built and validated with.  This is the BENCH build; CI
# uses arm-none-eabi.cmake for the SDK-free stub compile smoke.
#
# Expects tiarmclang on PATH (TI Code Composer Studio install, or the
# standalone ticlang package).  Set TICLANG_ROOT to the install prefix
# if the binaries are not on PATH.  Pin the exact version on the bench
# (record it in firmware/cc3501e/README.md "Build") so production images
# are reproducible.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(DEFINED ENV{TICLANG_ROOT})
    set(_ticlang_bin "$ENV{TICLANG_ROOT}/bin/")
else()
    set(_ticlang_bin "")
endif()

set(CMAKE_C_COMPILER   "${_ticlang_bin}tiarmclang")
set(CMAKE_ASM_COMPILER "${_ticlang_bin}tiarmclang")
set(CMAKE_OBJCOPY      "${_ticlang_bin}tiarmobjcopy" CACHE INTERNAL "")
set(CMAKE_SIZE         "${_ticlang_bin}tiarmsize"    CACHE INTERNAL "")
set(CMAKE_AR           "${_ticlang_bin}tiarmar"      CACHE INTERNAL "")

# Freestanding firmware image; don't try to run a probe executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_COMPILER_WORKS TRUE)
