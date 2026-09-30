/* link.ld -- A32 resident stub. Loaded (LMA) at 0x80020000 where TF-A
 * erets, linked (VMA) at STUB_BASE 0x0240C000 where start.S copies it.
 * objcopy -O binary emits the LMA image; `mkpayload.py release` may append a
 * payload after it (the stub never copies past __load_end). */
ENTRY(_vectors)

MEMORY
{
	MRAM (rx)  : ORIGIN = 0x80020000, LENGTH = 0x000E0000 /* mapped MRAM section 0x800 */
	SRAM (rwx) : ORIGIN = 0x0240C000, LENGTH = 0x00010000 /* up to STUB_LIMIT 0x0241C000 */
}

SECTIONS
{
	.image : {
		KEEP(*(.vectors))
		*(.text*)
		*(.rodata*)
		*(.data*)
		. = ALIGN(16);
	} > SRAM AT> MRAM
	__load_end = .;

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(4);
		__bss_end = .;
	} > SRAM

	/DISCARD/ : {
		*(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.note.*)
	}
}

ASSERT(_vectors == 0x0240C000, "link.ld: vectors must sit at STUB_BASE (re-entry ABI 0x0240C020/24)")
ASSERT(__bss_end <= 0x0241C000, "link.ld: stub exceeds 64 KiB (STUB_LIMIT, core 0 stack below)")
