/*
 * Copyright 2026 Alp Lab AB
 * Copyright (c) 2009-2020 Arm Limited. All rights reserved.
 * Copyright (c) 2026, GigaDevice Semiconductor Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linker script for the gd32-bridge OTA bootloader (Path A). Occupies the
 * first 32 KB of main flash (never erased by OTA); validates the active
 * slot's image and jumps to it. Layout matches src/ota_layout.h:
 *   bootloader 0x08000000 (32 KB) | metadata 0x08008000 (8 KB) | slots ...
 *
 * Derived from gd32g553_flash.ld; only the FLASH window differs.
 * HIL-REQUIRED: not validated on silicon (a bad bootloader bricks the
 * part — no host-driven SWD reflash this HW rev; recover via bench SWD).
 */

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    RAM      (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
    TCMSRAM  (xrw) : ORIGIN = 0x10000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

SECTIONS
{
    __stack_size = DEFINED(__stack_size) ? __stack_size : 2K;
    __heap_size  = DEFINED(__heap_size)  ? __heap_size  : 1K;

    .vectors :
    {
        . = ALIGN(4);
        KEEP(*(.vectors))
        . = ALIGN(4);
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

    .ARM.extab :   { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH
    .ARM.attributes : { *(.ARM.attributes) } > FLASH

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
}

GROUP(libgcc.a libc.a libm.a libnosys.a)
