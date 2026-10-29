ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		*(.text.head) *(.text*) *(.rodata*) *(.data*) *(.bss*) *(COMMON)
		. = ALIGN(16);
		. += 0x10000;
		_stack_top = .;
		BYTE(0)
	}
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
