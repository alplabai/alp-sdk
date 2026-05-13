/*
 * Copyright 2026 ALP Lab AB
 * Copyright (c) 2009-2020 Arm Limited. All rights reserved.
 * Copyright (c) 2026, GigaDevice Semiconductor Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linker script for the GD32G553MEY7TR on E1M-X V2N.  Memory layout:
 * 512 KB main flash (0x08000000), 96 KB SRAM (0x20000000) plus 32 KB
 * TCMSRAM (0x10000000) -- matches GD's `gd32g553_xE_flash.ld` reference
 * from the GD32G5x3 firmware library at
 * `Firmware/CMSIS/GD/GD32G5x3/Source/GCC/Ld/`.
 *
 * Differences vs the vendor reference:
 *
 *   * Empty weak `_init` / `_fini` symbols are emitted via the C
 *     translation unit `hal/gd32_libc_stubs.c` rather than the linker
 *     script (PROVIDE inside SECTIONS doesn't synthesise a real `bx lr`
 *     stub, only an address label that would alias the next instruction
 *     in `.text`).  newlib's `__libc_init_array` calls both; with
 *     `-nostartfiles` the toolchain's `crti.o` (which normally supplies
 *     these symbols) is dropped from the link.
 *
 *   * SPDX-License-Identifier tag (vendor file uses prose attribution
 *     only).
 *
 * The vendor file's structure -- section ordering, alignment rules,
 * `__stack_size` / `__heap_size` defaults, GROUP() of newlib archives --
 * is preserved verbatim so the runtime semantics match anything built
 * against the vendor's own template projects.
 */

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM      (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
    TCMSRAM  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

SECTIONS
{
    __stack_size = DEFINED(__stack_size) ? __stack_size : 2K;
    __heap_size  = DEFINED(__heap_size)  ? __heap_size  : 1K;

    /* Cortex-M vector table.  First word is the initial stack-pointer
     * value, second word is the Reset_Handler address.  The vendor's
     * `startup_gd32g5x3.S` emits the table into `.vectors`. */
    .vectors :
    {
        . = ALIGN(4);
        KEEP(*(.vectors))
        . = ALIGN(4);
        __Vectors_End  = .;
        __Vectors_Size = __Vectors_End - __gVectors;
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)
        KEEP (*(.init))
        KEEP (*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .ARM.attributes :
    {
        *(.ARM.attributes)
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array*))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT(.init_array.*)))
        KEEP (*(.init_array*))
        PROVIDE_HIDDEN (__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(.fini_array*))
        KEEP (*(SORT(.fini_array.*)))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } > FLASH

    /* `.data`: initialised globals.  Lives in RAM at runtime but is
     * loaded from FLASH at startup; `_sidata` is the load address in
     * FLASH, `_sdata` / `_edata` mark the runtime extent in RAM.
     * Reset_Handler copies the region byte-by-byte. */
    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > RAM

    .heap_stack :
    {
        . = ALIGN(8);
        PROVIDE ( end       = _ebss );
        PROVIDE ( _end      = _ebss );
        . = . + __heap_size;
        PROVIDE ( _heap_end = . );
        . = . + __stack_size;
        PROVIDE ( _sp       = . );
        . = ALIGN(8);
    } > RAM

    _sitcmsram = LOADADDR(.tcmsram);
    .tcmsram :
    {
        . = ALIGN(4);
        _stcmsram = .;
        *(.tcmsram)
        *(.tcmsram*)
        . = ALIGN(4);
        _etcmsram = .;
    } > TCMSRAM AT > FLASH
}

GROUP(libgcc.a libc.a libm.a libnosys.a)
