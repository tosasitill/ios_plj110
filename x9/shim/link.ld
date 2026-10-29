ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : {
		*(.data*)
		*(.bss*)
		*(COMMON)
		. = ALIGN(16);
		. += 0x10000;
		_stack_top = .;
		. += 0x4000;
		_exception_stack_top = .;
		BYTE(0)
	}
	. = ALIGN(4096);
	_shim_end = .;
	.payload : {
		_payload_start = .;
		KEEP(*(.payload))
		_payload_end = .;
	}
	. = ALIGN(4096);
	_image_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
