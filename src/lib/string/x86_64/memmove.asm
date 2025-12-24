section .text

global memmove:function
extern __memcpy_fwd:function

memmove:
    mov     rdi, rax
    sub     rsi, rax
    cmp     rdx, rax
    jae     __memcpy_fwd

    mov     rcx, rdx
    lea     rsi, [rsi + rdx - 1]
    lea     rdi, [rdi + rdx - 1]
    std
    rep     movsb  
    cld

    lea    rax, [rdi + 1]

    ret