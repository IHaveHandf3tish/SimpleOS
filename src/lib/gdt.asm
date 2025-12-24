section .text
global load_gdt

load_gdt:
    lgdt [rdi]
    
    ; Reload data segments  
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Reload CS with far return trick
    lea rax, [rel .reload_cs]
    push 0x08
    push rax
    retfq

.reload_cs:
    ret