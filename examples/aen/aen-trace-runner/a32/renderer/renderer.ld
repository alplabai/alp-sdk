/* renderer.ld -- flat renderer image at STUB_PAYLOAD_BASE. The .bin is .image
 * (+ .data's load copy right after it, objcopy lays out by LMA); only those
 * bytes are CRC'd. .data runs from a VMA past them and .bss follows, so HALT
 * then LAUNCH re-passes the CRC without a reload. Stack, DL, bins, setup and
 * bands live at fixed addresses (render.c, start.S), not here. Image + .bss
 * may run to 0x025C0000 (the bins, tr_memmap.h TR_MEM_A32_IMG_END); the image
 * alone (ctrl_len) stays within the resident stub's 512 KiB STUB_PAYLOAD_LIMIT. */
ENTRY(_start)
MEMORY { SRAM1 (rwx) : ORIGIN = 0x02500000, LENGTH = 0xC0000 }
SECTIONS
{
	.image : {
		KEEP(*(.text.start))
		*(.text*) *(.rodata*)
		. = ALIGN(8);
	} > SRAM1
	__data_load = LOADADDR(.image) + SIZEOF(.image);
	.data (__data_load + 0x1000) : AT(__data_load) {
		__data_start = .;
		*(.data*)
		. = ALIGN(4);
		__data_end = .;
	} > SRAM1
	.bss (NOLOAD) : {
		. = ALIGN(16);
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > SRAM1
	/DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.note.*) }
}
ASSERT(_start == 0x02500000, "entry must be the image base (ctrl_entry == CRC start)")
ASSERT(SIZEOF(.data) <= 0x1000, ".data VMA would overlap its own load copy")
ASSERT(__bss_end <= 0x025C0000, "image + .bss past 768 KiB (the bins start at 0x025C0000)")
ASSERT(SIZEOF(.image) + SIZEOF(.data) <= 0x80000, "image past the stub's 512 KiB STUB_PAYLOAD_LIMIT (LAUNCH would fault BAD_RANGE)")
