// P14ATL10E. Radius-26 Atlas: 10-bit values plus a sparse exception table.

#ifdef _WIN32
.section .rdata$p14_atlas,"dr"
#else
.section .rodata.p14_atlas,"a",@progbits
#endif
.balign 64
.global p14_atlas_begin
#ifndef _WIN32
.type p14_atlas_begin,@object
#endif
p14_atlas_begin:
.incbin "src/data/p14_atlas_r26_10.bin"
.global p14_atlas_end
p14_atlas_end:
// The decoder may perform a three-byte unaligned read for the final value.
.byte 0, 0

.balign 64
.global p14_atlas_exceptions_begin
#ifndef _WIN32
.type p14_atlas_exceptions_begin,@object
#endif
p14_atlas_exceptions_begin:
.incbin "src/data/p14_atlas_r26_exceptions.bin"
.global p14_atlas_exceptions_end
p14_atlas_exceptions_end:
#ifndef _WIN32
.size p14_atlas_begin, p14_atlas_end-p14_atlas_begin
.size p14_atlas_exceptions_begin, p14_atlas_exceptions_end-p14_atlas_exceptions_begin
.section .note.GNU-stack,"",@progbits
#endif
