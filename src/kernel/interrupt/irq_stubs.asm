[BITS 64]

global isr_stub_table
isr_stub_table:

extern irq_handler_c
extern irq_exception_c
extern irq_exception_ex

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

; Exception with error code: isr14 (Page Fault)
global isr14
isr14:
        PUSH_ALL
        ; // IRQスタブから直接プリエンプト用Cエントリを呼ぶ
        mov rdi, rsp            ; // 第1引数: スタックポインタ (保存されたレジスタ配列)
        mov rsi, 48             ; // 第2引数: ベクタ番号
        extern irq_preempt_entry
        call irq_preempt_entry
        POP_ALL
        iretq
        ; We don't have EIP in the same way, skip for now
        xor edx, edx           ; arg3: eip (placeholder)
        call irq_exception_ex
        POP_ALL
        add rsp, 8             ; Pop error code
        iretq

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
