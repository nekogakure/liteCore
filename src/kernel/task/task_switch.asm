; void task_switch(registers_t *old_regs, registers_t *new_regs)
global task_switch

section .text
bits 64

; registers_t 構造体のオフセット
%define REG_RAX 0
%define REG_RBX 8
%define REG_RCX 16
%define REG_RDX 24
%define REG_RSI 32
%define REG_RDI 40
%define REG_RBP 48
%define REG_RSP 56
%define REG_R8 64
%define REG_R9 72
%define REG_R10 80
%define REG_R11 88
%define REG_R12 96
%define REG_R13 104
%define REG_R14 112
%define REG_R15 120
%define REG_RIP 128
%define REG_RFLAGS 136
%define REG_CR3 144

task_switch:
    ; rdi = old_regs, rsi = new_regs
    
    ; いまのコンテキストを保存
    mov [rdi + REG_RAX], rax
    mov [rdi + REG_RBX], rbx
    mov [rdi + REG_RCX], rcx
    mov [rdi + REG_RDX], rdx
    mov [rdi + REG_RSI], rsi
    mov [rdi + REG_RDI], rdi
    mov [rdi + REG_RBP], rbp
    
    ; RSP は現在のスタックポインタを保存（call 後なので +8）
    mov rax, rsp
    add rax, 8                      ; return address分を調整
    mov [rdi + REG_RSP], rax
    
    mov [rdi + REG_R8], r8
    mov [rdi + REG_R9], r9
    mov [rdi + REG_R10], r10
    mov [rdi + REG_R11], r11
    mov [rdi + REG_R12], r12
    mov [rdi + REG_R13], r13
    mov [rdi + REG_R14], r14
    mov [rdi + REG_R15], r15
    
    ; RIP（戻りアドレス）を保存
    mov rax, [rsp]
    mov [rdi + REG_RIP], rax
    
    ; RFLAGS を保存
    pushfq
    pop rax
    mov [rdi + REG_RFLAGS], rax
    
    ; CR3 を保存
    mov rax, cr3
    mov [rdi + REG_CR3], rax
    
    ; === 新しいコンテキストに切り替える準備 ===
    ; new_regsのアドレスをr15に退避（r15は後で復元される）
    mov r15, rsi
    
    ; 新しいRIPとRSPを先に取得（CR3切り替え前）
    mov r14, [r15 + REG_RIP]
    mov r13, [r15 + REG_RSP]
    
    ; 新しい CR3 をロード（ページテーブル切り替え）
    mov rax, [r15 + REG_CR3]
    mov rbx, cr3
    cmp rax, rbx
    je .skip_cr3_load           ; 同じなら CR3 の再ロードはスキップ
    mov cr3, rax
.skip_cr3_load:
    
    ; RFLAGS を復元
    mov rax, [r15 + REG_RFLAGS]
    push rax
    popfq
    
    ; 汎用レジスタを復元（r13, r14, r15は後で）
    mov rax, [r15 + REG_RAX]
    mov rbx, [r15 + REG_RBX]
    mov rcx, [r15 + REG_RCX]
    mov rdx, [r15 + REG_RDX]
    mov rbp, [r15 + REG_RBP]
    mov r8,  [r15 + REG_R8]
    mov r9,  [r15 + REG_R9]
    mov r10, [r15 + REG_R10]
    mov r11, [r15 + REG_R11]
    mov r12, [r15 + REG_R12]
    
    ; RSI, RDI を復元
    mov rsi, [r15 + REG_RSI]
    mov rdi, [r15 + REG_RDI]
    
    ; 新しいスタックに切り替えてから戻り先をプッシュ
    mov rsp, r13
    push r14                    ; 新しいRIPをスタックにプッシュ
    
    ; r13, r14, r15 を復元
    mov r13, [r15 + REG_R13]
    mov r14, [r15 + REG_R14]
    mov r15, [r15 + REG_R15]
    
    ; 新しいタスクの RIP へジャンプ
    ret

; (task_restore: single definition follows)

; void task_restore(registers_t *new_regs)
global task_restore
task_restore:
    ; rdi = new_regs
    ; new_regs のアドレスを r15 に保持
    mov r15, rdi

    ; 新しい RIP と RSP を取得
    mov r14, [r15 + REG_RIP]
    mov r13, [r15 + REG_RSP]

    ; CR3 をロード（異なる場合のみ）
    mov rax, [r15 + REG_CR3]
    mov rbx, cr3
    cmp rax, rbx
    je .skip_cr3_load_restore
    mov cr3, rax
.skip_cr3_load_restore:

    ; RFLAGS を復元
    mov rax, [r15 + REG_RFLAGS]
    push rax
    popfq

    ; 汎用レジスタを復元
    mov rax, [r15 + REG_RAX]
    mov rbx, [r15 + REG_RBX]
    mov rcx, [r15 + REG_RCX]
    mov rdx, [r15 + REG_RDX]
    mov rbp, [r15 + REG_RBP]
    mov r8,  [r15 + REG_R8]
    mov r9,  [r15 + REG_R9]
    mov r10, [r15 + REG_R10]
    mov r11, [r15 + REG_R11]
    mov r12, [r15 + REG_R12]

    ; RSI, RDI を復元
    mov rsi, [r15 + REG_RSI]
    mov rdi, [r15 + REG_RDI]

    ; 新しいスタックに切り替え、戻り先(RIP)をプッシュ
    mov rsp, r13
    push r14

    ; r13, r14, r15 を復元（最後に r15 を復元しても良い）
    mov r13, [r15 + REG_R13]
    mov r14, [r15 + REG_R14]
    mov r15, [r15 + REG_R15]

    ; 新しいタスクの RIP へジャンプ
    ret

; void task_enter_usermode(uint64_t entry, uint64_t user_stack, uint64_t page_directory)
; ユーザーモードタスクへの初回起動用（iretqを使用）
global task_enter_usermode
task_enter_usermode:
    ; rdi = entry (RIP)
    ; rsi = user_stack (RSP)
    ; rdx = page_directory (CR3)
    cli  ; 割り込みを無効化
    
    ; iretq用にスタックフレームを構成
    ; スタック: SS, RSP, RFLAGS, CS, RIP の順（逆順でプッシュ）
    
    push 0x23      ; SS (ユーザーデータセグメント)
    push rsi       ; RSP (ユーザースタック)
    pushfq         ; 現在のRFLAGSを保存
    pop rax
    or rax, 0x200  ; IF=1を確実にセット
    push rax       ; RFLAGS
    push 0x1B      ; CS (ユーザーコードセグメント)
    push rdi       ; RIP (エントリポイント)
    
    ; CR3 をユーザのページディレクトリに切り替える
    mov cr3, rdx
    
    ; ユーザーモードへ移行
    iretq