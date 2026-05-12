/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Placeholder linker script for the GD32G553MEY7TR.
 *
 * Real values for the flash + RAM regions go in here once the
 * maintainer drops the GigaDevice firmware library; the chip's
 * memory layout (512 KB flash, 128 KB SRAM split across regions) is
 * documented in the GD32G5xx Reference Manual.
 *
 * The scaffold below carries minimum-viable region placeholders so
 * the CMake link step does not fail at configure time.  The bridge
 * firmware that needs to actually execute on silicon REQUIRES the
 * GigaDevice startup file (`startup_gd32g553xx.S`) + real region
 * sizes -- pull both in alongside the firmware library.
 */

ENTRY(Reset_Handler)

MEMORY
{
    /* TBD: pull authoritative region addresses + sizes from the
     * GD32G553xx datasheet / linker script that ships with the
     * GigaDevice firmware library. */
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
    RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text       : { *(.text*) }            > FLASH
    .rodata     : { *(.rodata*) }          > FLASH
    .data       : { *(.data*) }            > RAM AT > FLASH
    .bss        : { *(.bss*)  }            > RAM
}
