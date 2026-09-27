	.macro space64 x:req
		.if VER_64BIT == 1
			.space \x
		.endif
	.endm

	/* Aligns data that is read as pointers or as structs containing pointers.
	   Same as .align 2 on the GBA; on x86 .align counts bytes, not powers of 2. */
	.macro ptr_align
		.if VER_64BIT == 1
			.balign 8
		.else
			.balign 4
		.endif
	.endm

	.macro ptrvalue x:vararg
		.if VER_64BIT == 1
			.quad \x
		.else
			.int \x
		.endif
	.endm
