// P6SBM001. Link the compact P6 research tables into the submission executable without
// expanding them to multi-megabyte C++ initializer lists.

#include "data/p6_data_stamp.inc"

#ifdef _WIN32
.section .rdata$p6_stamp,"dr"
#else
.section .rodata.p6_stamp,"a",@progbits
#endif
.global p6_data_stamp
p6_data_stamp:
.asciz P6_DATA_STAMP

#ifdef _WIN32
.section .rdata$p6_axis,"dr"
#else
.section .rodata.p6_axis,"a",@progbits
#endif
.balign 64
.global p6_axis_begin
#ifndef _WIN32
.type p6_axis_begin,@object
#endif
p6_axis_begin:
.incbin "src/data/p6_axis_tables.bin"
.global p6_axis_end
p6_axis_end:
#ifndef _WIN32
.size p6_axis_begin, p6_axis_end-p6_axis_begin
#endif

#ifdef _WIN32
.section .rdata$p6_beams,"dr"
#else
.section .rodata.p6_beams,"a",@progbits
#endif
.balign 64
.global p6_beams_begin
#ifndef _WIN32
.type p6_beams_begin,@object
#endif
p6_beams_begin:
.incbin "src/data/p6_structured_beams.bin"
.global p6_beams_end
p6_beams_end:
#ifndef _WIN32
.size p6_beams_begin, p6_beams_end-p6_beams_begin
#endif

#ifdef _WIN32
.section .rdata$p6_top_edges,"dr"
#else
.section .rodata.p6_top_edges,"a",@progbits
#endif
.balign 64
.global p6_top_edges_begin
#ifndef _WIN32
.type p6_top_edges_begin,@object
#endif
p6_top_edges_begin:
.incbin "src/data/p6_top_edges.bin"
.global p6_top_edges_end
p6_top_edges_end:
#ifndef _WIN32
.size p6_top_edges_begin, p6_top_edges_end-p6_top_edges_begin

.section .note.GNU-stack,"",@progbits
#endif
