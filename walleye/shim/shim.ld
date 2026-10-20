ENTRY(_head)
SECTIONS
{
	. = 0;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	. = ALIGN(16);
	_bss_start = .;
	.bss : { *(.bss*) *(COMMON) }
	. = ALIGN(16);
	_bss_end = .;
	. += 0x8000;
	. = ALIGN(16);
	_stack_top = .;
	. = ALIGN(4096);
	_sec_stacks = .;
	. += 8 * 0x1000;
	/* last: start.S reaches .bss and the stacks with adr (+-1MB) */
	. = ALIGN(16);
	.payload : { *(.payload) }
	_end = .;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
