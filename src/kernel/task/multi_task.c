#include <task/multi_task.h>
#include <util/console.h>
#include <mem/manager.h>
#include <mem/paging.h>
#include <mem/vmem.h>
#include <mem/map.h>
#include <interrupt/irq.h>
#include <stddef.h>
#include <string.h>
#include <fs/vfs.h>

// IRQ から直接タスクを復元するためのアセンブリ関数
extern void task_restore(registers_t *new_regs);

#define MAX_TASKS 64
#define KERNEL_STACK_SIZE 0x4000 // 16KB
#define USER_STACK_SIZE 0x4000 // 16KB
#define TIME_SLICE_DEFAULT 10 // 10 ticks

static task_t *tasks[MAX_TASKS];
static task_t *current_task = NULL;
static task_t *ready_queue_head = NULL;
static task_t *ready_queue_tail = NULL;
static uint32_t next_tid = 1;
static int scheduler_enabled = 0;

// idle タスク（常に実行可能）
static task_t idle_task;

/*
static void idle_task_entry(void) {
	while (1) {
		asm volatile("hlt");
	}
}
*/

/**
 * @brief 文字列コピー（簡易実装）
 */
static void str_copy(char *dst, const char *src, size_t max_len) {
	size_t i = 0;
	while (i < max_len - 1 && src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

/**
 * @brief タスク用のページディレクトリを作成
 * カーネル空間を共有、ユーザー空間は独立
 * @return ページディレクトリの物理アドレス、失敗時は0
 */
static uint64_t create_task_page_directory(void) {
	// 新しいユーザープロセス用のPML4を作成
	// カーネル空間のマッピングは自動的にコピーされる
	uint64_t new_pml4 = paging64_create_user_pml4();

	if (new_pml4 == 0) {
		printk("create_task_page_directory: Failed to create user PML4\n");
		// フォールバック: カーネルPML4を使用
		uint64_t kernel_cr3;
		asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
		return kernel_cr3;
	}

	printk("create_task_page_directory: Created new PML4=0x%016lx\n",
	       new_pml4);

	return new_pml4;
}

/**
 * @brief タスクシステムを初期化
 */
void task_init(void) {
#ifdef INIT_MSG
	printk("task_init: Initializing multitasking system...\n");
#endif
	for (int i = 0; i < MAX_TASKS; i++) {
		tasks[i] = NULL;
	}

	ready_queue_head = NULL;
	ready_queue_tail = NULL;

	// idle タスクを初期化（TID=0）
	// idle タスクは現在実行中のコンテキスト（kmain）として初期化
	idle_task.tid = 0;
	str_copy(idle_task.name, "idle", sizeof(idle_task.name));
	idle_task.state = TASK_STATE_RUNNING; // 既に実行中
	idle_task.kernel_mode = 1; // カーネルモード
	idle_task.time_slice = TIME_SLICE_DEFAULT;
	idle_task.total_time = 0;
	idle_task.next = NULL;

	// idle タスクは現在のページディレクトリを使用
	uint64_t current_cr3;
	asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
	idle_task.page_directory = current_cr3;
	idle_task.regs.cr3 = current_cr3;

	// idle タスクのレジスタは現在のコンテキストを保持
	// （スタックポインタは現在のRSP、RIPは戻り先）
	// これらはタスクスイッチ時に正しく保存される
	uint64_t current_rsp;
	asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
	idle_task.regs.rsp = current_rsp;
	idle_task.regs.rip = 0; // task_switch から戻る時に設定される
	idle_task.regs.rflags = 0x202; // IF=1 (割り込み有効)

	// 他のレジスタをゼロ初期化
	idle_task.regs.rax = idle_task.regs.rbx = idle_task.regs.rcx =
		idle_task.regs.rdx = 0;
	idle_task.regs.rsi = idle_task.regs.rdi = idle_task.regs.rbp = 0;
	idle_task.regs.r8 = idle_task.regs.r9 = idle_task.regs.r10 =
		idle_task.regs.r11 = 0;
	idle_task.regs.r12 = idle_task.regs.r13 = idle_task.regs.r14 =
		idle_task.regs.r15 = 0;

	// カーネルスタックは現在のスタックをそのまま使用
	idle_task.kernel_stack = current_rsp;
	idle_task.user_stack = 0;

	current_task = &idle_task;
	tasks[0] = &idle_task;

	scheduler_enabled = 1;

	for (int i = 0; i < 32; ++i) {
		idle_task.fds[i] = -1;
	}
	idle_task.fds[0] = 0; /* stdin */
	idle_task.fds[1] = 1; /* stdout */
	idle_task.fds[2] = 2; /* stderr */
	vfs_init();
#ifdef INIT_MSG
	printk("task_init: Multitasking initialized. Current context saved as idle task (TID=0, CR3=0x%lx)\n",
	       (unsigned long)idle_task.regs.cr3);
#endif
}

/**
 * @brief 新しいタスクを作成
 */
task_t *task_create(void (*entry)(void), const char *name, int kernel_mode) {
	if (!scheduler_enabled) {
		printk("task_create: Scheduler not initialized\n");
		return NULL;
	}

	// 空きスロットを探す
	int slot = -1;
	for (int i = 1; i < MAX_TASKS; i++) {
		if (tasks[i] == NULL) {
			slot = i;
			break;
		}
	}

	if (slot == -1) {
		printk("task_create: No free task slots\n");
		return NULL;
	}

	// タスク構造体を確保
	task_t *task = (task_t *)alloc_frame();
	if (!task) {
		printk("task_create: Failed to allocate task structure\n");
		return NULL;
	}

	// タスク構造体を仮想アドレスに変換
	uint32_t task_virt = vmem_phys_to_virt((uint32_t)(uintptr_t)task);
	task = (task_t *)(uintptr_t)task_virt;

	// タスク構造体を初期化
	task->tid = next_tid++;
	str_copy(task->name, name, sizeof(task->name));
	task->state = TASK_STATE_READY;
	task->kernel_mode = kernel_mode; // カーネルモードフラグを設定
	task->time_slice = TIME_SLICE_DEFAULT;
	task->total_time = 0;
	task->next = NULL;

	// カーネルスタックを確保
	void *kstack = alloc_frame();
	if (!kstack) {
		printk("task_create: Failed to allocate kernel stack\n");
		return NULL;
	}
	uint32_t kstack_virt =
		vmem_phys_to_virt((uint32_t)(uintptr_t)kstack) + 0x1000;
	task->kernel_stack = kstack_virt;

	if (kernel_mode) {
		// カーネルモードタスク: 現在のページディレクトリを共有
		uint64_t current_cr3_val;
		asm volatile("mov %%cr3, %0" : "=r"(current_cr3_val));
		task->page_directory = current_cr3_val;
		task->user_stack = 0;

		/* init per-task brk */
		task->user_brk = 0;
		task->user_brk_size = 0;

		// スタックの初期化：entry関数が戻る先としてtask_exitをpush
		// task_switch は ret 命令で task->regs.rip にジャンプする
		// entry 関数が ret で戻る時、スタックの task_exit にジャンプする
		uint64_t *stack_ptr = (uint64_t *)(uintptr_t)
			kstack_virt; // スタック最上位から開始
		stack_ptr--; // 8バイト下げる
		*stack_ptr = (uint64_t)task_exit; // entry から戻る先

		// レジスタを初期化
		task->regs.rsp = (uint64_t)stack_ptr;
		task->regs.rip =
			(uint64_t)entry; // task_switch が ret でここにジャンプ
		task->regs.rflags = 0x202; // IF=1
		task->regs.cr3 = current_cr3_val;
	} else {
		// ユーザーモードタスク: 独立したページディレクトリを作成
		uint64_t pd_phys = create_task_page_directory();
		if (pd_phys == 0) {
			printk("task_create: Failed to create page directory\n");
			return NULL;
		}
		task->page_directory = pd_phys;

		/* ユーザースタックを確保
		 * 暫定実装: 64ビットページング対応のため、カーネル空間のアドレスを使用
		 * TODO: 適切な64ビットページマッピングを実装
		 */
		void *ustack = alloc_frame();
		if (!ustack) {
			printk("task_create: Failed to allocate user stack\n");
			return NULL;
		}
		uint32_t ustack_phys = (uint32_t)(uintptr_t)ustack;

		/* カーネル仮想アドレスに変換してゼロクリア */
		uint32_t frame_virt = vmem_phys_to_virt(ustack_phys);
		if (frame_virt != 0) {
			memset((void *)(uintptr_t)frame_virt, 0, 0x1000);
		}

		/* user_stackには物理アドレスを保存（ELFローダーが使用） */
		task->user_stack = (uint64_t)ustack_phys;

		printk("task_create: user_stack set to 0x%lx (phys=0x%x virt=0x%x)\n",
		       task->user_stack, ustack_phys, frame_virt);

		/* init per-task brk */
		printk("task_create: About to set user_brk\n");
		task->user_brk = 0;
		task->user_brk_size = 0;

		// レジスタを初期化
		printk("task_create: About to initialize registers\n");
		task->regs.rsp = task->user_stack;
		task->regs.rip = (uint64_t)entry;
		task->regs.rflags = 0x202; // IF=1
		printk("task_create: About to set cr3=0x%lx\n", pd_phys);
		task->regs.cr3 = pd_phys;
		printk("task_create: Registers initialized\n");
	}

	// 汎用レジスタをゼロクリア
	task->regs.rax = task->regs.rbx = task->regs.rcx = task->regs.rdx = 0;
	task->regs.rsi = task->regs.rdi = task->regs.rbp = 0;
	task->regs.r8 = task->regs.r9 = task->regs.r10 = task->regs.r11 = 0;
	task->regs.r12 = task->regs.r13 = task->regs.r14 = task->regs.r15 = 0;

	/* initialize per-task fd table */
	for (int i = 0; i < 32; ++i)
		task->fds[i] = -1;
	/* inherit standard fds for kernel-mode tasks */
	if (kernel_mode) {
		task->fds[0] = 0;
		task->fds[1] = 1;
		task->fds[2] = 2;
	}

	tasks[slot] = task;

#ifdef INIT_MSG
	printk("task_create: Created task '%s' (TID=%u)\n", name,
	       (unsigned)task->tid);
#endif

	return task;
}

/**
 * @brief タスクをレディキューに追加
 */
void task_ready(task_t *task) {
	if (!task)
		return;

	uint64_t flags = irq_save();

	task->state = TASK_STATE_READY;
	task->next = NULL;

	if (ready_queue_tail) {
		ready_queue_tail->next = task;
		ready_queue_tail = task;
	} else {
		ready_queue_head = ready_queue_tail = task;
	}

	irq_restore(flags);
}

/**
 * @brief 現在実行中のタスクを取得
 */
task_t *task_current(void) {
	return current_task;
}

/**
 * @brief スケジューラを実行（ラウンドロビン）
 */
void task_schedule(void) {
	if (!scheduler_enabled || !current_task)
		return;

	uint64_t flags = irq_save();

	task_t *next_task = NULL;

	// レディキューから次のタスクを取得
	if (ready_queue_head) {
		next_task = ready_queue_head;
		ready_queue_head = next_task->next;
		if (ready_queue_head == NULL) {
			ready_queue_tail = NULL;
		}
		next_task->next = NULL;
	}

	// 次のタスクがなければidleタスクに戻る（現在のタスクがDEADまたはidleでない場合）
	if (!next_task) {
		if (current_task->state == TASK_STATE_DEAD ||
		    current_task != &idle_task) {
			next_task = &idle_task;
		} else {
			// 現在のタスクがidleで、他に実行可能なタスクがない場合は継続
			irq_restore(flags);
			return;
		}
	}

	if (current_task->state == TASK_STATE_RUNNING) {
		current_task->state = TASK_STATE_READY;
		if (ready_queue_tail) {
			ready_queue_tail->next = current_task;
			ready_queue_tail = current_task;
		} else {
			ready_queue_head = ready_queue_tail = current_task;
		}
		current_task->next = NULL;
	}

	task_t *old_task = current_task;
	current_task = next_task;
	next_task->state = TASK_STATE_RUNNING;

	irq_restore(flags);

	// コンテキストスイッチ（異なるタスクの場合のみ）
	if (old_task != next_task) {
		task_switch(&old_task->regs, &next_task->regs);
	}
}

/**
 * @brief 現在のタスクを終了
 */
void task_exit(void) {
	uint64_t flags = irq_save();

	if (current_task) {
		current_task->state = TASK_STATE_DEAD;
	}

	irq_restore(flags);

	// スケジューラを呼んで次のタスクへ
	task_schedule();

	while (1) {
		asm volatile("hlt");
	}
}

/**
 * @brief タスクを自発的に譲る
 */
void task_yield(void) {
	task_schedule();
}

/**
 * @brief IRQ コンテキストから呼ばれるスケジューラ
 * IRQ により current_task->regs は事前に保存されている想定
 * （例: irq_timer_entry が regs_stack を TCB に書き込む）
 */
void task_schedule_from_irq(registers_t *irq_regs) {
	if (!scheduler_enabled || !current_task)
		return;

	task_t *next_task = NULL;

	/* レディキューから次のタスクを取得 */
	if (ready_queue_head) {
		next_task = ready_queue_head;
		ready_queue_head = next_task->next;
		if (ready_queue_head == NULL) {
			ready_queue_tail = NULL;
		}
		next_task->next = NULL;
	}

	if (!next_task) {
		if (current_task->state == TASK_STATE_DEAD ||
		    current_task != &idle_task) {
			next_task = &idle_task;
		} else {
			/* 現在のタスクが idle で他に実行可能なタスクがない */
			return;
		}
	}

	if (current_task->state == TASK_STATE_RUNNING) {
		current_task->state = TASK_STATE_READY;
		if (ready_queue_tail) {
			ready_queue_tail->next = current_task;
			ready_queue_tail = current_task;
		} else {
			ready_queue_head = ready_queue_tail = current_task;
		}
		current_task->next = NULL;
	}

	task_t *old_task = current_task;
	current_task = next_task;
	next_task->state = TASK_STATE_RUNNING;

	if (old_task != next_task) {
		(void)irq_regs;
		task_restore(&next_task->regs);
	}
}
