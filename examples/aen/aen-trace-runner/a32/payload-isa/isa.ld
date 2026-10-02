/* isa.ld -- a32/common/payload.ld with room for the golden-frame buffers:
 * .bss may run to 0x025F0000 (the renderer's DL/bins/stacks MiB-1 tail: it is
 * halted and rebuilds all of it at its next LAUNCH; 0x025FE000, its core-1
 * gate word, stays untouched). */
ENTRY(_start)
MEMORY { SRAM1 (rwx) : ORIGIN = 0x02500000, LENGTH = 0xF0000 }
SECTIONS
{
	.image : {
		KEEP(*(.text.start))
		*(.text*) *(.rodata*)
		. = ALIGN(4);
	} > SRAM1
	.data : { *(.data*) } > SRAM1
	.bss (NOLOAD) : {
		. = ALIGN(64);
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(8);
		. += 0x2000;
		__stack_top = .;
		__bss_end = .;
	} > SRAM1
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.note.*) }
}
ASSERT(_start == 0x02500000, "entry must be the image base (ctrl_entry == CRC start)")
ASSERT(SIZEOF(.data) == 0, ".data would be mutated inside the CRC'd image")
ASSERT(SIZEOF(.image) <= 0x80000, "image over the stub's 512 KiB payload budget")
