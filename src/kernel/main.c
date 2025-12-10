#include <util/config.h>
#include <util/console.h>
#include <device/pci.h>
#include <device/keyboard.h>
#include <interrupt/irq.h>
#include <interrupt/idt.h>
#include <shell/commands.h>
#include <shell/shell.h>
#include <shell/shell_integration.h>
#include <util/io.h>
#include <util/init_msg.h>
#include <util/debug.h>
#include <mem/map.h>
#include <mem/manager.h>
#include <mem/segment.h>
#include <mem/tss.h>
#include <driver/ata.h>
#include <fs/fat/fat16.h>
#include <driver/timer/timer.h>
#include <boot_info.h>
#include <task/multi_task.h>

#include <tests/define.h>
#include <tests/run.h>

void kloop();

// グローバルFAT16ハンドル
struct fat16_super *g_fat16_sb = NULL;

// グローバルブート情報
static BOOT_INFO *g_boot_info = NULL;

/**
 * @fn kmain
 * @brief LiteCoreのメイン関数（kernel_entryより）
 */
void kmain(BOOT_INFO *boot_info) {
	g_boot_info = boot_info;

	console_set_framebuffer(boot_info);
	console_init();
	set_log_level(ALL);

	gdt_build();
	gdt_install_lgdt();
	gdt_install_jump();

	kernel_init();

	printk("Welcome to Litecore kernel!\n");
	printk("    Version : %s\n", VERSION);
	printk("    Build   : %s %s\n", __DATE__, __TIME__);
	printk("    Author  : nekogakure\n");

#ifdef TEST_TRUE
	new_line();
	printk("====== TESTS ======\n");
	run_test();
#endif /* TEST_TRUE */

	new_line();

	new_line();

	printk("Startup process complete :D\n");
	printk("initializing shell...\n");

	init_full_shell();

	__asm__ volatile("sti");

	while (1) {
		kloop();
	}
}

/**
 * @fn kloop
 * @brief kmainの処理が終了した後常に動き続ける処理
 */
void kloop() {
	int activity =
		0; // このループで何か処理したかフラグ（分かりづらい仕事しろ、）

	/* FIFOに入ったイベントを処理 */
	int event_count = 0;
	while (interrupt_dispatch_one()) {
		activity = 1;
		event_count++;
	}

	shell_readline_and_execute();

	task_yield();

	/* 何も処理しなかった場合はCPUを休止（次の割り込みまで） */
	if (!activity) {
		cpu_halt();
	}
}