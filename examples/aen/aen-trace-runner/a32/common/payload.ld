/* payload.ld -- flat stub payload (colour-bar, echo) at STUB_PAYLOAD_BASE. Only
 * .image is loaded and CRC'd; .bss (stack included) sits after it so HALT
 * then LAUNCH re-passes the CRC without a reload. */
ENTRY(_start)
MEMORY { SRAM1 (rwx) : ORIGIN = 0x02500000, LENGTH = 0x80000 }
SECTIONS
{
	.image : {
		KEEP(*(.text.start))
		*(.text*) *(.rodata*)
		. = ALIGN(4);
	} > SRAM1
	.data : { *(.data*) } > SRAM1
	.bss (NOLOAD) : {
		. = ALIGN(8);
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
