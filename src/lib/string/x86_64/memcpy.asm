section .text

global __memcpy_fwd:function hidden
global memcpy:function

memcpy:
__memcpy_fwd:
    mov     rax, rdi
    cmp     rdx, 8
    jc      .setup_bulk
    test    edi, 7
    jz      .setup_bulk

.align_loop:
    movsb
    dec     rdx
    test    edi, 7
    jnz     .align_loop

.setup_bulk:
    mov     rcx, rdx
    shr     rcx, 3
    rep movsq
    and     edx, 7
    jz      .done

.tail_loop:
    movsb
    dec     edx
    jnz     .tail_loop

.done:
    ret
