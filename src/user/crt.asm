bits 64

section .text
global _start
extern main
extern _impure_data
extern _impure_ptr
extern __stack_chk_init

_start:
    xor rbp, rbp
    xor rdi, rdi
    xor rsi, rsi
    lea rax, [rel _impure_data]
    mov [rel _impure_ptr], rax
    mov rax, 158
    mov rdi, 0x1002
    lea rsi, [rel tls_area]
    syscall
    call __stack_chk_init
    

    call run_init_array

    call main
    
    mov rdi, rax
    mov rax, 60
    syscall

.halt:
    jmp .halt

run_init_array:
    push rbx
    extern __init_array_start
    extern __init_array_end
    lea rbx, [rel __init_array_start]
    lea rcx, [rel __init_array_end]
    cmp rbx, rcx
    jae .done
.loop:
    mov rax, [rbx]
    test rax, rax
    jz .skip
    call rax
.skip:
    add rbx, 8
    cmp rbx, rcx
    jb .loop
.done:
    pop rbx
    ret

section .data
align 16
tls_area:
    times 40 db 0
    dq _impure_data
    times 216 db 0
