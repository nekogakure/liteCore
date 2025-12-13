[BITS 64]

global isr_stub_table
isr_stub_table:

extern irq_handler_c
extern irq_exception_ex
extern last_isr_stack

; Macro for saving all registers (64-bit)
%macro PUSH_ALL 0
        push rax
        push rcx
        push rdx
        push rbx
        push rbp
        push rsi
        push rdi
        push r8
        push r9
        push r10
        push r11
        push r12
        push r13
        push r14
        push r15
%endmacro

; Macro for restoring all registers (64-bit)
%macro POP_ALL 0
        pop r15
        pop r14
        pop r13
        pop r12
        pop r11
        pop r10
        pop r9
        pop r8
        pop rdi
        pop rsi
        pop rbp
        pop rbx
        pop rdx
        pop rcx
        pop rax
%endmacro

; Exception handlers (0-31)
; Exceptions without error code
%macro ISR_NOERR 1
global isr%1
isr%1:
        cli                     ; Disable interrupts
        push qword 0            ; Dummy error code
        push qword %1           ; Push vector number
        jmp isr_common_stub
%endmacro

; Exceptions with error code
%macro ISR_ERR 1
global isr%1
isr%1:
        cli                     ; Disable interrupts
        push qword %1           ; Push vector number (error code already on stack)
        jmp isr_common_stub_err
%endmacro

; Common stub for exceptions without error code
isr_common_stub:
        PUSH_ALL
        ; save pointer to the ISR stack (rsp after PUSH_ALL) into C-visible variable
        mov rax, rsp
        mov [rel last_isr_stack], rax
        mov rdi, [rsp + 15*8]   ; Vector number
        xor esi, esi            ; Error code = 0
        call irq_exception_ex
        POP_ALL
        add rsp, 16             ; Pop vector number and dummy error code
        iretq

; Common stub for exceptions with error code
isr_common_stub_err:
        PUSH_ALL
        ; save pointer to the ISR stack (rsp after PUSH_ALL) into C-visible variable
        mov rax, rsp
        mov [rel last_isr_stack], rax
        mov rdi, [rsp + 15*8]   ; Vector number
        mov rsi, [rsp + 16*8]   ; Error code
        call irq_exception_ex
        POP_ALL
        add rsp, 16             ; Pop vector number and error code
        iretq

; CPU Exceptions (0-31)
ISR_NOERR 0   ; Divide by Zero
ISR_NOERR 1   ; Debug
ISR_NOERR 2   ; Non-Maskable Interrupt
ISR_NOERR 3   ; Breakpoint
ISR_NOERR 4   ; Overflow
ISR_NOERR 5   ; Bound Range Exceeded
ISR_NOERR 6   ; Invalid Opcode
ISR_NOERR 7   ; Device Not Available
ISR_ERR   8   ; Double Fault
ISR_NOERR 9   ; Coprocessor Segment Overrun (legacy)
ISR_ERR   10  ; Invalid TSS
ISR_ERR   11  ; Segment Not Present
ISR_ERR   12  ; Stack-Segment Fault
ISR_ERR   13  ; General Protection Fault
ISR_ERR   14  ; Page Fault
ISR_NOERR 15  ; Reserved
ISR_NOERR 16  ; x87 Floating-Point Exception
ISR_ERR   17  ; Alignment Check
ISR_NOERR 18  ; Machine Check
ISR_NOERR 19  ; SIMD Floating-Point Exception
ISR_NOERR 20  ; Virtualization Exception
ISR_ERR   21  ; Control Protection Exception
ISR_NOERR 22  ; Reserved
ISR_NOERR 23  ; Reserved
ISR_NOERR 24  ; Reserved
ISR_NOERR 25  ; Reserved
ISR_NOERR 26  ; Reserved
ISR_NOERR 27  ; Reserved
ISR_NOERR 28  ; Hypervisor Injection Exception
ISR_ERR   29  ; VMM Communication Exception
ISR_ERR   30  ; Security Exception
ISR_NOERR 31  ; Reserved

; IRQ handlers 32-47
global isr32
isr32:
        PUSH_ALL
        mov edi, 32
        call irq_handler_c
        POP_ALL
        iretq

global isr33
isr33:
        PUSH_ALL
        mov edi, 33
        call irq_handler_c
        POP_ALL
        iretq

global isr34
isr34:
        PUSH_ALL
        mov edi, 34
        call irq_handler_c
        POP_ALL
        iretq

global isr35
isr35:
        PUSH_ALL
        mov edi, 35
        call irq_handler_c
        POP_ALL
        iretq

global isr36
isr36:
        PUSH_ALL
        mov edi, 36
        call irq_handler_c
        POP_ALL
        iretq

global isr37
isr37:
        PUSH_ALL
        mov edi, 37
        call irq_handler_c
        POP_ALL
        iretq

global isr38
isr38:
        PUSH_ALL
        mov edi, 38
        call irq_handler_c
        POP_ALL
        iretq

global isr39
isr39:
        PUSH_ALL
        mov edi, 39
        call irq_handler_c
        POP_ALL
        iretq

global isr40
isr40:
        PUSH_ALL
        mov edi, 40
        call irq_handler_c
        POP_ALL
        iretq

global isr41
isr41:
        PUSH_ALL
        mov edi, 41
        call irq_handler_c
        POP_ALL
        iretq

global isr42
isr42:
        PUSH_ALL
        mov edi, 42
        call irq_handler_c
        POP_ALL
        iretq

global isr43
isr43:
        PUSH_ALL
        mov edi, 43
        call irq_handler_c
        POP_ALL
        iretq

global isr44
isr44:
        PUSH_ALL
        mov edi, 44
        call irq_handler_c
        POP_ALL
        iretq

global isr45
isr45:
        PUSH_ALL
        mov edi, 45
        call irq_handler_c
        POP_ALL
        iretq

global isr46
isr46:
        PUSH_ALL
        mov edi, 46
        call irq_handler_c
        POP_ALL
        iretq

global isr47
isr47:
        PUSH_ALL
        mov edi, 47
        call irq_handler_c
        POP_ALL
        iretq

global isr48
isr48:
        PUSH_ALL
        mov rdi, rsp
        extern irq_timer_entry
        call irq_timer_entry
        POP_ALL
        iretq

global isr128
isr128:
        PUSH_ALL
        mov rdi, rsp
        mov rsi, 128
        extern syscall_entry_c
        call syscall_entry_c
        POP_ALL
        iretq
