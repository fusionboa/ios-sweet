/* The SWEETXNU payload (tools/mkpayload.py), linked into the Image; see loader.c. */
	.section .payload, "a"
	.balign	16
	.globl	_payload_start
_payload_start:
#ifdef PAYLOAD
	.incbin	PAYLOAD
#endif
	.balign	16
	.globl	_payload_end
_payload_end:
