	.arch armv6k
	.section .init, "ax", %progbits
	.arm
	.syntax unified
	.align  2
	.global _start
	.type   _start, %function
_start:
	ldr     r12, .L0
	sub     r0, sp, 0x280 @ spi is not that stack hungry.... so...
	str     r0, [r12]
	mov     r0, #0
	mov     r12, #0
	bl      SPIMain
	svc     0x03
.L0:
	.word   _thread_stack_sp_top_offset
	.size   _start, .-_start

	.section .text._thread_start, "ax", %progbits
	.align  2
	.global _thread_start
	.type   _thread_start, %function
_thread_start:
	ldmdb   sp, {r0,r1} @ pop function and argument
	blx     r1 @ call function with argument set
	svc     0x09

	.section .data._thread_stack_sp_top_offset, "aw"
	.align  2
	.global _thread_stack_sp_top_offset
_thread_stack_sp_top_offset:
	.word 0
	.size   _thread_stack_sp_top_offset, .-_thread_stack_sp_top_offset
