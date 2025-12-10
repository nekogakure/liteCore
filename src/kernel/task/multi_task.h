#ifndef _TASK_TASK_H
#define _TASK_TASK_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief タスク状態
 */
typedef enum {
	TASK_STATE_READY = 0, // 実行可能
	TASK_STATE_RUNNING = 1, // 実行中
	TASK_STATE_BLOCKED = 2, // ブロック中
	TASK_STATE_DEAD = 3 // 終了済み
} task_state_t;

/**
 * @brief レジスタコンテキスト（x86-64）
 * コンテキストスイッチ時に保存/復元されるレジスタ群
 */
typedef struct {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip; // 命令ポインタ
	uint64_t rflags; // フラグレジスタ
	uint64_t cr3; // ページディレクトリベースレジスタ
} registers_t;

/**
 * @brief タスク制御ブロック（TCB）
 */
typedef struct task {
	uint32_t tid; // タスクID
	char name[32]; // タスク名
	task_state_t state; // タスク状態
	int kernel_mode; // カーネルモードフラグ（1=カーネル、0=ユーザー）
	registers_t regs; // レジスタコンテキスト
	uint64_t kernel_stack; // カーネルスタックポインタ
	uint64_t user_stack; // ユーザースタックポインタ
	uint64_t page_directory; // ページディレクトリの物理アドレス

	uint64_t user_brk;
	uint64_t user_brk_size;
	uint64_t time_slice; // タイムスライス（ticks）
	uint64_t total_time; // 累計実行時間（ticks）
	struct task *next; // 次のタスク（リンクリスト）
	int fds[32]; // per-taskファイルディスクリプタテーブル
} task_t;

/**
 * @brief タスクシステムを初期化
 */
void task_init(void);

/**
 * @brief 新しいタスクを作成
 * @param entry タスクのエントリポイント
 * @param name タスク名
 * @param kernel_mode カーネルモードで実行する場合は1、ユーザーモードは0
 * @return 作成されたタスクのポインタ、失敗時はNULL
 */
task_t *task_create(void (*entry)(void), const char *name, int kernel_mode);

/**
 * @brief タスクをレディキューに追加
 * @param task 追加するタスク
 */
void task_ready(task_t *task);

/**
 * @brief 現在実行中のタスクを取得
 * @return 現在のタスクポインタ
 */
task_t *task_current(void);

/**
 * @brief スケジューラを実行（次のタスクに切り替え）
 * この関数はタイマー割り込みハンドラから呼ばれる
 */
void task_schedule(void);

/**
 * @brief 現在のタスクを終了
 */
void task_exit(void);

/**
 * @brief タスクを自発的に譲る（yield）
 */
void task_yield(void);

/**
 * @brief コンテキストスイッチを実行（アセンブリ実装）
 * @param old_regs 現在のタスクのレジスタ保存先
 * @param new_regs 次のタスクのレジスタ
 */
extern void task_switch(registers_t *old_regs, registers_t *new_regs);

void task_schedule_from_irq(registers_t *irq_regs);

extern void task_restore(registers_t *new_regs);

/**
 * @brief ユーザーモードタスクへ初回起動（アセンブリ実装）
 * @param entry エントリポイント（RIP）
 * @param user_stack ユーザースタックポインタ（RSP）
 * @param page_directory ユーザーページディレクトリ（CR3）
 */
extern void task_enter_usermode(uint64_t entry, uint64_t user_stack,
				uint64_t page_directory);

#endif /* _TASK_TASK_H */
