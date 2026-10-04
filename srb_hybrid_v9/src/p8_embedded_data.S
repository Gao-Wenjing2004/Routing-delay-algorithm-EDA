// P8SBM001. Embed the medium-distance macro transition tables into the binary.

#include "data/p8_data_stamp.inc"

#ifdef _WIN32
.section .rdata$p8_stamp,"dr"
#else
.section .rodata.p8_stamp,"a",@progbits
#endif
.global p8_data_stamp
p8_data_stamp:
.asciz P8_DATA_STAMP

#ifdef _WIN32
.section .rdata$p8_axis,"dr"
#else
.section .rodata.p8_axis,"a",@progbits
#endif
.balign 64
.global p8_axis_begin
#ifndef _WIN32
.type p8_axis_begin,@object
#endif
p8_axis_begin:
.incbin "src/data/p8_axis_tables.bin"
.global p8_axis_end
p8_axis_end:
#ifndef _WIN32
.size p8_axis_begin, p8_axis_end-p8_axis_begin
#endif

#ifdef _WIN32
.section .rdata$p8_beams,"dr"
#else
.section .rodata.p8_beams,"a",@progbits
#endif
.balign 64
.global p8_beams_begin
#ifndef _WIN32
.type p8_beams_begin,@object
#endif
p8_beams_begin:
.incbin "src/data/p8_macro_beams.bin"
.global p8_beams_end
p8_beams_end:
#ifndef _WIN32
.size p8_beams_begin, p8_beams_end-p8_beams_begin
#endif

#ifdef _WIN32
.section .rdata$p8_top_edges,"dr"
#else
.section .rodata.p8_top_edges,"a",@progbits
#endif
.balign 64
.global p8_top_edges_begin
#ifndef _WIN32
.type p8_top_edges_begin,@object
#endif
p8_top_edges_begin:
.incbin "src/data/p8_top_edges.bin"
.global p8_top_edges_end
p8_top_edges_end:
#ifndef _WIN32
.size p8_top_edges_begin, p8_top_edges_end-p8_top_edges_begin

.section .note.GNU-stack,"",@progbits
#endif
