/* Stand-in XNU entry: MMU off, EL1, x0=boot_args PA. */
	.section .text.head, "ax"
	.globl _start
_start:
	msr daifset, #0xf
	msr spsel, #1
	adrp x1, _stack_top
	add x1, x1, :lo12:_stack_top
	mov sp, x1
	bl stub_main
1:
	wfe
	b 1b
