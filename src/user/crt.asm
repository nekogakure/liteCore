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
    ; set up a tiny TLS area and set FS base so newlib's TLS-based _REENT access works
    mov rax, 158                   ; syscall: arch_prctl
    mov rdi, 0x1002                ; ARCH_SET_FS
    lea rsi, [rel tls_area]
    syscall                        ; Use syscall instruction
    call __stack_chk_init
    call main
    mov rdi, rax
    mov rax, 2
    syscall                        ; exit syscall

.halt:
    ; Don't use hlt (privileged instruction)
    ; Just loop indefinitely
    jmp .halt

section .data
align 16
tls_area:
    times 40 db 0
    dq _impure_data
    times 216 db 0
