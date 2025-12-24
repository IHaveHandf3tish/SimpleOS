section .text
extern exception_handler 
global load_idt
global isr1

; 1. The IDT Loader Function
load_idt:
    lidt [rdi] 
    ;for now, we won't enable interrupts here because i haven't set up the PIC yet
    sti
    ret

; 2. The Generic Interrupt Service Routine (ISR)
; When an interrupt happens, the CPU jumps here
isr1:
    push 0                  ; Push a dummy error code (so stack is consistent)
    push 1                  ; Push the interrupt number (1 = Debug Exception)
    
    ; Save all registers (C functions might overwrite them)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Call handler
    call exception_handler
    
    ; Restore ALL registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    ; Clean up the error code and interrupt number we pushed
    add rsp, 16 

    iretq ; "Interrupt Return" - Go back to what the kernel was doing

extern page_fault_handler

    global isr_page_fault
isr_page_fault:
    ; CPU pushes error code automatically
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    ; ... save other registers
    
    mov rdi, [rsp + 120]  ; Error code
    mov rax, cr2         ; Fault address
    mov rsi, rax
    
    call page_fault_handler
    
    ; Restore and return
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 8  ; Pop error code
    iretq
