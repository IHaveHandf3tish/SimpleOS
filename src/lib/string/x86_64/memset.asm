global memset:function

memset:
    movzx   rax, sil
    mov     r8, 0x0101010101010101
    imul    rax, r8

    cmp     rdx, 126
    ja      .L_large_entry

    test    edx, edx
    jz      .L_return

    mov     [rdi], sil
    mov     [rdi + rdx - 1], sil
    cmp     edx, 2
    jbe     .L_return

    mov     [rdi + 1], ax
    mov     [rdi + rdx - 3], ax
    cmp     edx, 6
    jbe     .L_return

    mov     [rdi + 3], eax
    mov     [rdi + rdx - 7], eax
    cmp     edx, 14
    jbe     .L_return

    mov     [rdi + 7], rax
    mov     [rdi + rdx - 15], rax
    cmp     edx, 30
    jbe     .L_return

    mov     [rdi + 15], rax
    mov     [rdi + 23], rax
    mov     [rdi + rdx - 31], rax
    mov     [rdi + rdx - 15], rax
    cmp     edx, 62
    jbe     .L_return

    mov     [rdi + 31], rax
    mov     [rdi + 39], rax
    mov     [rdi + 47], rax
    mov     [rdi + 55], rax
    mov     [rdi + rdx - 63], rax
    mov     [rdi + rdx - 55], rax
    mov     [rdi + rdx - 47], rax
    mov     [rdi + rdx - 39], rax

.L_return:
    mov     rax, rdi
    ret

.L_large_entry:
    test    edi, 15
    mov     r8, rdi
    mov     [rdi + rdx - 8], rax
    mov     rcx, rdx
    jnz     .L_calc_align

.L_aligned_loop:
    shr     rcx, 3
    rep stosq
    mov     rax, r8
    ret

.L_calc_align:
    xor     edx, edx
    sub     edx, edi
    and     edx, 15
    mov     [rdi], rax
    mov     [rdi + 8], rax
    sub     rcx, rdx
    add     rdi, rdx
    jmp     .L_aligned_loop