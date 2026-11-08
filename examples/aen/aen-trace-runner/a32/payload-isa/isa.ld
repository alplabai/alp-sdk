/* isa.ld -- a32/common/payload.ld with room for the golden-frame buffers:
 * .bss may run to 0x025E0000 (the renderer's stacks, 0x025C0000..0x025DFFFF: it is
 * halted and rebuilds them at its next LAUNCH; 0x025E0000, its core-1 gate
 * word, and FB B from 0x025EA000 stay untouched). */
ENTRY(_start)
MEMORY { SRAM1 (rwx) : ORIGIN = 0x02500000, LENGTH = 0xE0000 }
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
