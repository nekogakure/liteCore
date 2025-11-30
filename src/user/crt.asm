bits 64

section .text
global _start
extern main

_start:
    xor rbp, rbp
    xor rdi, rdi
    xor rsi, rsi
    call main
    mov rdi, rax
    mov rax, 2
    int 0x80

.halt:
    hlt
    jmp .halt
