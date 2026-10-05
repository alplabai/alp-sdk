<!-- SPDX-License-Identifier: Apache-2.0 -->
# Upstream PR draft: micro_ros_zephyr_module on Zephyr 4.4 (picolibc)

Draft for micro-ROS/micro_ros_zephyr_module, answering issue #158 ("Support
for Zephyr 4.2 and above"). **Not opened.** The patch is
`examples/multicore/microros-ros2-v2n/m33_sm/patches/0002-microros-zephyr-4.4-picolibc-build.patch`;
it applies to the module at `8477de12` with or without the alp-sdk-only
`0001` (custom transport) patch. alp-sdk issue: #375.

## Title

libmicroros: build against Zephyr >= 4.3 with the picolibc toolchain

## Description

The module's CI covers Zephyr 4.0 and 4.1 only. On Zephyr 4.4 with Zephyr
SDK 1.0.1 (picolibc is the default libc, `CONFIG_POSIX_API=y`) the colcon
cross-build of the ROS 2 packages stops at the first package, and after
fixing that the link fails. Nothing in the transports needed to change
(a custom transport compiles no module transport source).

`libmicroros.mk` already derives the ROS package flags from the Zephyr
application build (`zephyr_get_*_for_lang_as_string`). That is the right
design and is kept; the patch removes only what is specific to compiling
Zephyr's own sources, and adds what picolibc needs.

| # | First error | Cause | Change |
|---|---|---|---|
| 1 | `string.h: unknown type name '__errno_t'` / `'__rsize_t'` (rcutils `allocator.c`) | `rcutils/error_handling.h` defines `__STDC_WANT_LIB_EXT1__ 1` after `<stdlib.h>`/`<stdio.h>`. picolibc's `<sys/_types.h>` (include-guarded) emits `__errno_t`/`__rsize_t` only if the macro is set on its first inclusion, while `<string.h>`/`<stdlib.h>` test the macro again later. | `-D__STDC_WANT_LIB_EXT1__=0` on the command line (picolibc has no Annex K; rcutils guards its define with `#ifndef`). Also exported to consumers through `target_compile_definitions(microros INTERFACE ...)`: the application's own `main.c` hits the same error via the installed rcutils headers. |
| 2 | `rcutils/src/strcasecmp.c: implicit declaration of function 'strcasecmp'` | Zephyr compiles with `-D_POSIX_C_SOURCE=200809L` (`lib/posix/options/CMakeLists.txt`). picolibc then exposes strict POSIX only, `<string.h>` stops including `<strings.h>`. The ROS sources assume the libc default feature set. | `-D_DEFAULT_SOURCE` |
| 3 | `fatal error: zephyr/posix/time.h: No such file or directory` (rcutils `time_unix.c`) | Zephyr 4.3 removed `include/zephyr/posix/time.h` and `signal.h` (zephyr `5cbb2a421d9`, "posix: switch to using posix_time.h and posix_signal.h"). rcutils includes it for every Zephyr >= 3.1. | `modules/libmicroros/compat/zephyr/posix/time.h` forwarding to `<time.h>`, appended last on the include path so an older Zephyr's real header wins. The root fix is a version gate in `micro-ROS/rcutils` `time_unix.c` (`ZEPHYR_VERSION_CODE >= 4.3.0` -> `<time.h>`); the compat header stays until that is released. |
| 4 | `rcutils/src/logging.c: implicit declaration of function 'isatty'` | `CONFIG_POSIX_SYSTEM_INTERFACES` (selected by `CONFIG_POSIX_API`) adds `include/zephyr/posix` with `zephyr_include_directories()`, a plain `-I`. Zephyr's `<unistd.h>` then shadows the C library's and has no `isatty()`. | Filter `-I.../include/zephyr/posix` out of the flags handed to colcon. The ROS packages are plain C written against the libc; the libc's POSIX headers come from the sysroot. The hardcoded `-I$(ZEPHYR_BASE)/include/posix` is dropped too: Zephyr has had no such directory since before 4.1. |
| 5 | `ld.bfd: GNU-style constructors required but STATIC_INIT_GNU not enabled` | `libmicroros.a` contains C++ objects (`rosidl_typesupport_c` `*.cpp` type supports) with `.init_array`. `STATIC_INIT_GNU` defaults to `y` only with `CONFIG_CPP`, which the sample sets and a plain-C application does not. | `select STATIC_INIT_GNU if TOOLCHAIN_SUPPORTS_STATIC_INIT_GNU` under `MICROROS` |

No header of Zephyr or of the toolchain is modified.

## Verification

Zephyr v4.4.0, Zephyr SDK 1.0.1 (`arm-zephyr-eabi`, GCC 14.3.0, picolibc),
Ubuntu 24.04 (WSL2). Application: a publisher on a custom transport,
`CONFIG_POSIX_API=y`, `CONFIG_MICROROS_TRANSPORT_CUSTOM=y`.

- Cortex-M33 (`r9a09g056n48gbg/cm33`): colcon `Summary: 62 packages finished`,
  link to `zephyr.elf`.
- Build only. Nothing was flashed or run.

## Not covered

- Runtime: static initializers, `clock_gettime`, stack and heap sizing.
- newlib (`CONFIG_NEWLIB_LIBC=y`), the module's serial and UDP transports,
  native_sim.
- The module clones ROS sources at branch heads, so the result holds for the
  heads of the day.
- The module builds into its own source directory and colcon caches the
  CMake flags in `micro_ros_src/build`; changing board or Kconfig needs
  `make -f libmicroros.mk clean`. Pre-existing, not changed here.
