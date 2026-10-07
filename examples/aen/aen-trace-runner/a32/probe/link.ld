/* link.ld -- Cortex-A32 bare-metal probe, XIP from MRAM.
 *
 * TF-A sp_min erets straight to 0x80020000 (PRELOADED_BL33_BASE) and
 * executes the first word there -- no image header, no loader copy. So the
 * image must be placed exactly at 0x80020000 with the vector table as its
 * first bytes, and it must carry no .data/.bss: there is nothing that
 * copies a data section out of MRAM into RAM for us, and MRAM itself is
 * used read-only (see docs/2026-09-22-a32-probe-spec.md, "Memory plan").
 * All mutable state lives in the results block / stack in SRAM0, set up by
 * start.S -- never in a linker-placed section.
 */
ENTRY(_vectors)

MEMORY
{
	MRAM (rx) : ORIGIN = 0x80020000, LENGTH = 0x005E0000 /* up to 0x805FFFFF per the memory plan */
}

SECTIONS
{
	. = 0x80020000;

	.text : {
		KEEP(*(.vectors))
		. = ALIGN(4);
		*(.text*)
	} > MRAM

	.rodata : {
		*(.rodata*)
	} > MRAM

	/DISCARD/ : {
		*(.comment)
		*(.ARM.attributes)
		*(.ARM.exidx*)
		*(.ARM.extab*)
		*(.note.*)
	}

	/* No mutable state may ship in the XIP image: assert both are empty
	 * rather than silently placing them in MRAM (which nothing zeroes/
	 * copies, and which is used read-only by contract). */
	.data : { *(.data*) } > MRAM
	.bss  : { *(.bss*) *(COMMON) } > MRAM
}

ASSERT(SIZEOF(.data) == 0, "link.ld: .data must be empty -- image is XIP read-only MRAM, nothing copies it")
ASSERT(SIZEOF(.bss) == 0, "link.ld: .bss must be empty -- image is XIP read-only MRAM, nothing zeroes it")
