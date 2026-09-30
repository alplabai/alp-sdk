/* a32/renderer/start.S -- renderer image entry at STUB_PAYLOAD_BASE, per the
 * stub contract (a32/common/stub_abi.h). Runs at EVERY LAUNCH:
 *   core 1 -> own stack (MiB 1, WB), FPSCR = 0, renderer_core1 (waits for
 *   core 0's gate, switches to the renderer table core 0 built, then
 *   setup halves and bands); on return -> STUB_REENTER_CORE1, which
 *   restores the stub table;
 *   core 0 -> own stack (MiB 1, WB), FPSCR = 0, .data from its pristine
 *   load copy, .bss zeroed, renderer table at RENDER_TTB, renderer_main;
 *   on return (HALT consumed) -> STUB_REENTER_CORE0, which restores the stub
 *   table itself. The stub vectors stay live: a fault records fault_* and
 *   parks (the contract allows keeping them). */
#include "stub_abi.h"

	.equ RENDER_TTB,       0x02408000
	.equ RENDER_STACK0_TOP, 0x025EE000   @ tr_memmap.h TR_MEM_A32_STACKS 0x025DE000 + 64 KiB (core 0)
	.equ RENDER_STACK1_TOP, 0x025FE000   @ + 64 KiB (core 1)

	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	cmp r0, #0
	bne core1
	ldr sp, =RENDER_STACK0_TOP
	mov r4, r1                      @ &mbox
	mov r0, #0
	vmsr fpscr, r0                  @ RN, FZ=0, DN=0: what the host goldens assume

	ldr r0, =__data_start           @ .data: VMA past the CRC'd image, copied every entry
	ldr r1, =__data_load
	ldr r2, =__data_end
1:	cmp r0, r2
	ldrlo r3, [r1], #4
	strlo r3, [r0], #4
	blo 1b
	ldr r0, =__bss_start
	ldr r1, =__bss_end
	mov r2, #0
2:	cmp r0, r1
	strlo r2, [r0], #4
	blo 2b

	bl  render_build_table          @ C, writes RENDER_TTB (NC page)
	dsb sy
	ldr r0, =RENDER_TTB             @ low bits 0: NC walks, like the stub
	mcr p15, 0, r0, c2, c0, 0       @ TTBR0 (TTBCR/DACR/SCTLR as the stub left them)
	isb
	mov r0, #0
	mcr p15, 0, r0, c8, c7, 0       @ TLBIALL
	mcr p15, 0, r0, c7, c5, 6       @ BPIALL
	dsb sy
	isb

	mov r0, r4
	bl  renderer_main               @ returns after consuming HALT
	ldr pc, =STUB_REENTER_CORE0

core1:
	ldr sp, =RENDER_STACK1_TOP
	mov r0, #0
	vmsr fpscr, r0
	mov r0, r1                      @ &mbox
	bl  renderer_core1              @ returns on HALT (before core 0 does)
	ldr pc, =STUB_REENTER_CORE1
	.ltorg
