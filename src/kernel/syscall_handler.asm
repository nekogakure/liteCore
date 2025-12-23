global syscall_handler
extern syscall_entry_c

section .text

syscall_handler:
    ; RCX = user RIP (return address)
    ; R11 = user RFLAGS
    ; RSP = user stack pointer
    
    ; Save user context in scratch registers
    ; We need to preserve: RCX (RIP), R11 (RFLAGS), RSP
    mov r15, rsp        ; Save user RSP
    
    ; Switch to kernel stack
    lea rsp, [rel kernel_stack_top]
    
    ; Save user state on kernel stack for sysret
    push r15            ; User RSP
    push r11            ; User RFLAGS
    push rcx            ; User RIP
    
    ; Build register frame for syscall_entry_c
    ; Expected layout: [rax, rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10]
    push r10            ; syscall arg3 (r10 instead of rcx for syscall convention)
    push r9             ; syscall arg5
    push r8             ; syscall arg4
    push rdi            ; syscall arg0
    push rsi            ; syscall arg1
    push rbp
    push rbx
    push rdx            ; syscall arg2
    push rcx            ; save user RIP in rcx slot
    push rax            ; syscall number
    
    ; Call C handler with register pointer
    mov rdi, rsp
    xor esi, esi
    call syscall_entry_c
    
    ; Return value is in stack[0] (was rax position)
    ; Pop register frame (discard most values except return value)
    pop rax             ; return value (was syscall number position)
    add rsp, 72         ; skip rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10 (9 * 8 = 72)
    
    ; Restore user state for sysret
    pop rcx             ; User RIP
    pop r11             ; User RFLAGS
    pop rsp             ; User RSP
    
    ; Return to userspace
    ; SYSRET expects: RCX = RIP, R11 = RFLAGS, RSP = user stack
    o64 sysret

section .bss
align 16
kernel_stack: resb 8192    ; 8KB kernel stack for syscall
kernel_stack_top:          ; Top of the stack (grows downward)
