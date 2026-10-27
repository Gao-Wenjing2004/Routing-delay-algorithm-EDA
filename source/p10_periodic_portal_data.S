    .section .rodata.srb_p10_portal,"a",@progbits
    .balign 4096
    .global srb_p10_up_begin
    .type srb_p10_up_begin,@object
srb_p10_up_begin:
    .incbin "analysis/unique_eval/p10_rebuild/up/packed_1_5.bin"
    .global srb_p10_up_end
    .type srb_p10_up_end,@object
srb_p10_up_end:
    .balign 4096
    .global srb_p10_down_begin
    .type srb_p10_down_begin,@object
srb_p10_down_begin:
    .incbin "analysis/unique_eval/p10_rebuild/down/packed_1_5.bin"
    .global srb_p10_down_end
    .type srb_p10_down_end,@object
srb_p10_down_end:
    .section .note.GNU-stack,"",@progbits
