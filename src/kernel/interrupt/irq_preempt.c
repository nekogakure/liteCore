#include <interrupt/irq.h>
#include <task/multi_task.h>
#include <driver/timer/apic.h>
#include <util/console.h>
#include <stdint.h>
#include "irq_trace.h"

// スタックレイアウト (regs_stack のインデックス):
// 0: RAX
// 1: RCX
// 2: RDX
// 3: RBX
// 4: RBP
// 5: RSI
// 6: RDI
// 7: R8
// 8: R9
// 9: R10
// 10: R11
// 11: R12
// 12: R13
// 13: R14
// 14: R15
// 15: RIP (CPUが積む戻り先)
// 16: CS
// 17: RFLAGS

void irq_preempt_entry(uint64_t *regs_stack, uint32_t vec) {
	// 現在タスクを取得
	task_t *t = task_current();

	if (t) {
		// 保存可能な汎用レジスタを TCB に格納
		t->regs.rax = regs_stack[0];
		t->regs.rcx = regs_stack[1];
		t->regs.rdx = regs_stack[2];
		t->regs.rbx = regs_stack[3];
		t->regs.rbp = regs_stack[4];
		t->regs.rsi = regs_stack[5];
		t->regs.rdi = regs_stack[6];
		t->regs.r8 = regs_stack[7];
		t->regs.r9 = regs_stack[8];
		t->regs.r10 = regs_stack[9];
		t->regs.r11 = regs_stack[10];
		t->regs.r12 = regs_stack[11];
		t->regs.r13 = regs_stack[12];
		t->regs.r14 = regs_stack[13];
		t->regs.r15 = regs_stack[14];

		// RIP と RFLAGS はスタック上の定位置から取得
		t->regs.rip = regs_stack[15];
		t->regs.rflags = regs_stack[17];

		t->regs.rsp = (uint64_t)(&regs_stack[15]) + 8;

		// CR3 を読み取って保存
		uint64_t cr3;
		asm volatile("mov %%cr3, %0" : "=r"(cr3));
		t->regs.cr3 = cr3;
	}

	irq_trace_record_from_stack(regs_stack, vec);

	// タイマ割り込みベクタならタイマーハンドラを即時実行
	if (vec == 48) {
		// APIC タイマの同期処理
		extern void apic_timer_tick(uint32_t, void *);
		apic_timer_tick(0, NULL);
	}

	// スケジューラに制御を渡す（IRQ コンテキストからの切替）
	if (t) {
		task_schedule_from_irq(&t->regs);
	}
}