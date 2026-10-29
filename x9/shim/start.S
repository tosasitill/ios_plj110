/*
 * Direct LK entry, not an EFI application. The PE-compatible ARM64 Image
 * header preserves the stock MZ prefix but branches to OUR entry at +4.
 * Never preserve the stock branch to an absent +0x2121068 trampoline.
 *
 * Linux arm64 boot contract: x0=FDT, MMU off, EL1 or EL2. Unknown mappings
 * are not guessed and faults are not skipped. A non-contract MMU-on entry
 * stops here instead of executing C through invented LK virtual aliases.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	.ascii "MZ@"
	.byte 0xfa
	b _start
	.quad 0
	.quad _image_end
	.quad 0xa
	.quad 0, 0, 0
	.ascii "ARM\x64"
	.long pe_header - _head
pe_header:
	.ascii "PE\0\0"
	.short 0xaa64, 1
	.long 0, 0, 0
	.short 240, 0x22
	.short 0x20b
	.byte 0, 0
	.long _image_end - 0x1000, 0, 0
	.long _start - _head, 0x1000
	.quad 0
	.long 0x1000, 0x200
	.short 0, 0, 0, 0, 0, 0
	.long 0
	.long _image_end, 0x200, 0
	.short 10, 0
	.quad 0x10000, 0x4000, 0x10000, 0x4000
	.long 0, 16
	.zero 16 * 8
	.ascii ".text\0\0\0"
	.long _image_end - 0x1000, 0x1000
	.long _image_end - 0x1000, 0x1000
	.long 0, 0
	.short 0, 0
	.long 0xe0000060
	.balign 0x1000
	.globl _start
_start:
	msr daifset, #0xf
	mov x19, x0
	mrs x20, CurrentEL
	cmp x20, #4
	b.eq el1_check
	cmp x20, #8
	b.ne entry_stop
	mrs x0, sctlr_el2
	tbnz x0, #0, entry_stop
	/* Normalise EL2 -> EL1 first. RW=1 selects AArch64 at EL1. Clearing
	 * E2H/TGE also changes what VBAR_EL2 and SCTLR_EL2 mean: under VHE,
	 * VBAR_EL2 is the EL1&0 vector base, so the EL2 vectors below must be
	 * installed after this write. The SCTLR check above covers both
	 * layouts because under E2H it is the EL1&0 MMU bit. */
	mov x0, #(1 << 31)
	msr hcr_el2, x0
	isb
	/* A trap from here to ERET must report here, not run firmware vectors. */
	adrp x0, el2_vectors
	add x0, x0, :lo12:el2_vectors
	msr vbar_el2, x0
	isb
	/* FEAT_FGT: firmware may leave traps that redirect cache maintenance
	 * or the register writes below. Clear them for EL1's benefit. */
	mrs x0, id_aa64mmfr0_el1
	ubfx x1, x0, #56, #4
	cbz x1, el2_no_fgt
	.arch armv8.6-a
	msr hfgitr_el2, xzr
	msr hfgrtr_el2, xzr
	msr hfgwtr_el2, xzr
	.arch armv8-a
	isb
el2_no_fgt:
	mov x0, #3
	msr cnthctl_el2, x0
	msr cntvoff_el2, xzr
	mov x0, #0x0800
	movk x0, #0x30d0, lsl #16
	msr sctlr_el1, x0
	msr cptr_el2, xzr
	/* MIDR_EL1/MPIDR_EL1 reads at EL1 are redirected through these; their
	 * reset values are unknown, so publish the identification XNU reads. */
	mrs x0, midr_el1
	msr vpidr_el2, x0
	mrs x0, mpidr_el1
	msr vmpidr_el2, x0
	/* XNU's pe_fiq.c uses ICC_SRE_EL1/ICC_PMR_EL1 directly. Without this,
	 * those EL1 accesses trap back to EL2 instead of reaching the GIC. */
	mrs x0, id_aa64pfr0_el1
	ubfx x1, x0, #24, #4
	cbz x1, el2_no_gic
	mrs x0, icc_sre_el2
	orr x0, x0, #1
	orr x0, x0, #8
	msr icc_sre_el2, x0
	isb
	msr ich_hcr_el2, xzr
el2_no_gic:
	mov x0, #0x3c5
	msr spsr_el2, x0
	adr x0, el1_entry
	msr elr_el2, x0
	isb
	eret
el1_check:
	mrs x0, sctlr_el1
	tbnz x0, #0, entry_stop
el1_entry:
	/* Cache maintenance is defined at EL1; DC CISW/IC IALLU are not
	 * defined at EL2 when HCR_EL2.TGE is clear. */
	bl clean_dcache
	msr spsel, #1
	adrp x0, _stack_top
	add x0, x0, :lo12:_stack_top
	mov sp, x0
	adrp x0, vectors
	add x0, x0, :lo12:vectors
	msr vbar_el1, x0
	mrs x0, sctlr_el1
	bic x0, x0, #4
	orr x0, x0, #(1 << 12)
	msr sctlr_el1, x0
	isb
	mov x0, x19
	lsr x1, x20, #2
	bl shim_main
entry_stop:
	wfe
	b entry_stop

/* No stack needed; support both legacy and FEAT_CCIDX cache geometry. */
clean_dcache:
	dsb sy
	mrs x0, clidr_el1
	ubfx x3, x0, #24, #3
	lsl x3, x3, #1
	cbz x3, 6f
	mrs x15, id_aa64mmfr2_el1
	ubfx x15, x15, #20, #4
	mov x10, #0
3:
	add x2, x10, x10, lsr #1
	lsr x1, x0, x2
	and x1, x1, #7
	cmp x1, #2
	b.lt 5f
	msr csselr_el1, x10
	isb
	mrs x1, ccsidr_el1
	and x2, x1, #7
	add x2, x2, #4
	cbnz x15, 7f
	ubfx x4, x1, #3, #10
	ubfx x7, x1, #13, #15
	b 8f
7:
	ubfx x4, x1, #3, #21
	ubfx x7, x1, #32, #24
8:
	clz w5, w4
9:
	mov x9, x4
4:
	lsl x12, x9, x5
	lsl x13, x7, x2
	orr x12, x12, x13
	orr x12, x12, x10
	dc cisw, x12
	subs x9, x9, #1
	b.ge 4b
	subs x7, x7, #1
	b.ge 9b
5:
	add x10, x10, #2
	cmp x10, x3
	b.lt 3b
6:
	msr csselr_el1, xzr
	dsb sy
	ic iallu
	dsb sy
	isb
	ret

/* All exception classes terminate with a report; no ELR += 4 harness. */
	.balign 0x800
vectors:
	.rept 16
	b exception_entry
	.balign 0x80
	.endr
exception_entry:
	msr daifset, #0xf
	mrs x0, esr_el1
	mrs x1, elr_el1
	mrs x2, far_el1
	adrp x3, _exception_stack_top
	add x3, x3, :lo12:_exception_stack_top
	mov sp, x3
	bl shim_exception
	b entry_stop

/* Same report for an EL2 trap before the ERET; without this VBAR_EL2 still
 * points at firmware vectors and a fault would restart that firmware. */
	.balign 0x800
el2_vectors:
	.rept 16
	b el2_exception_entry
	.balign 0x80
	.endr
el2_exception_entry:
	msr daifset, #0xf
	msr spsel, #1
	mrs x0, esr_el2
	mrs x1, elr_el2
	mrs x2, far_el2
	adrp x3, _exception_stack_top
	add x3, x3, :lo12:_exception_stack_top
	mov sp, x3
	bl shim_el2_exception
	b entry_stop

	.globl xnu_enter
xnu_enter:
	mov x16, x0
	mov x0, x1
	mov x1, xzr
	mov x2, xzr
	mov x3, xzr
	dsb sy
	ic iallu
	dsb sy
	mrs x4, sctlr_el1
	bic x4, x4, #1
	bic x4, x4, #4
	bic x4, x4, #(1 << 12)
	msr sctlr_el1, x4
	isb
	br x16
