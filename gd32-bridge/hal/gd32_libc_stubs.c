/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Newlib runtime stubs needed by the GD32 backend's `-nostartfiles`
 * link.  newlib's `__libc_init_array` (called from the vendor's
 * `Reset_Handler` in `startup_gd32g5x3.S`) walks the `.preinit_array` /
 * `.init_array` ranges and invokes `_init()` between the two -- by
 * default the toolchain supplies a NOP definition via `crti.o`, but
 * `-nostartfiles` drops `crti.o` / `crtn.o` along with the C runtime
 * startup files, leaving `_init` / `_fini` undefined at link time.
 *
 * Bridge firmware uses neither C++ static initialisation nor classic-
 * style init/fini sections, so the bodies stay intentionally empty.
 * Definitions are weak so a future commit can swap in real bodies (for
 * instance, an SoC-pin-mux initialiser) without source-side changes.
 *
 * Only linked when `BRIDGE_HAL_BACKEND=gd32` -- the stub backend never
 * pulls in the vendor `Reset_Handler` and so never references `_init` /
 * `_fini`.
 */

__attribute__((weak)) void _init(void)
{
}
__attribute__((weak)) void _fini(void)
{
}
