    .section .rodata.srb_exact_local,"a",@progbits
    .balign 4096
    .global srb_exact_local_begin
    .type srb_exact_local_begin,@object
srb_exact_local_begin:
    .incbin "analysis/unique_eval/local_route_packing/exact_local_route_table.bin"
    .global srb_exact_local_end
    .type srb_exact_local_end,@object
srb_exact_local_end:
    .section .note.GNU-stack,"",@progbits
