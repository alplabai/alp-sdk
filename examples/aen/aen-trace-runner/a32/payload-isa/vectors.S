/* vectors.S -- payload-isa exception vectors: an UNDEF (a CP15 register NS
 * PL1 may not read) is counted and skipped (`movs pc, lr`: ARM state, LR_und
 * = the instruction + 4). Every other vector goes to the stub's own handler
 * (record + park, stub_abi.h "Faults"). The stub's re-entry path restores
 * VBAR, so nothing needs undoing. */
#include "stub_abi.h"

	.syntax unified
	.arm
	.text
	.align 5
isa_vectors:
	ldr pc, =(STUB_BASE + 0x00)
	b   undef
	ldr pc, =(STUB_BASE + 0x08)
	ldr pc, =(STUB_BASE + 0x0C)
	ldr pc, =(STUB_BASE + 0x10)
	ldr pc, =(STUB_BASE + 0x14)
	ldr pc, =(STUB_BASE + 0x18)
	ldr pc, =(STUB_BASE + 0x1C)

undef:
	push {r0-r3, r12, lr}
	bl  isa_undef_record
	pop {r0-r3, r12, lr}
	movs pc, lr
	.ltorg

/* void isa_install_vectors(void): undef-mode stack, then VBAR. */
	.global isa_install_vectors
isa_install_vectors:
	mrs r0, cpsr
	cps #0x1B                      @ UND
	ldr sp, =__und_stack_top
	msr cpsr_c, r0                 @ back to the caller's mode, masks unchanged
	ldr r0, =isa_vectors
	mcr p15, 0, r0, c12, c0, 0     @ VBAR
	isb
	bx  lr
	.ltorg

	.bss
	.align 3
	.space 512
__und_stack_top:
