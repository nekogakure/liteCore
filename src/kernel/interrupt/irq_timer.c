#include <util/config.h>
#include <interrupt/irq.h>
#include <task/multi_task.h>
#include <driver/timer/apic.h>
#include <stdint.h>

// IRQ 経路から呼ばれるスケジューラ関数（TCB のレジスタポインタを渡す）
extern void task_schedule_from_irq(registers_t *irq_regs);

// irq_stubs から呼ばれる。regs_stack は PUSH_ALL 直後の RSP を指す。
void irq_timer_entry(uint64_t *regs_stack) {
	// タイマー状態を更新（APIC タイマのカウンタを進める）
	apic_timer_tick(48, NULL);

	// 現在のタスクのレジスタスナップショットに保存する
	task_t *t = task_current();
	if (t) {
		// PUSH_ALL の順序に合わせて値を取り出す
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

		// CPU がプッシュした RIP/CS/RFLAGS は regs_stack[15..17] にある
		t->regs.rip = regs_stack[15];
		t->regs.rflags = regs_stack[17];

		uintptr_t saved_rsp =
			(uintptr_t)regs_stack + (size_t)((15 + 3) * 8);
		t->regs.rsp = saved_rsp;

		// CR3 は現在の CR3 を読み取って保存
		uint64_t cr3val;
		asm volatile("mov %%cr3, %0" : "=r"(cr3val));
		t->regs.cr3 = cr3val;
	}

	task_schedule_from_irq(&t->regs);
}
