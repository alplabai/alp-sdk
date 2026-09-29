/* payload_start.S -- common entry for flat payloads at STUB_PAYLOAD_BASE (colour-bar, echo).
 * Honours the stub contract (a32/common/stub_abi.h): entered at
 * STUB_PAYLOAD_BASE with r0 = core id, r1 = &mbox, running on the stub's
 * table and vectors, which it keeps -- so nothing needs restoring on return. */
#include "stub_abi.h"

	.syntax unified
	.arm
	.section .text.start, "ax"
	.global _start
_start:
	cmp r0, #0
	ldrne pc, =STUB_REENTER_CORE1   @ core 1 has no work here: straight back to its park
	ldr sp, =__stack_top            @ own stack (.bss, outside the CRC'd image)
	ldr r2, =__bss_start
	ldr r3, =__bss_end
	mov r0, #0
1:	cmp r2, r3
	strlo r0, [r2], #4
	blo 1b
	mov r0, r1                      @ &mbox
	bl  payload_main                @ returns after consuming HALT
	ldr pc, =STUB_REENTER_CORE0
	.ltorg
