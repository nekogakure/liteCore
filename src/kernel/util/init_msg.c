#include <util/config.h>
#include <util/console.h>
#include <util/bdf.h>
#include <device/pci.h>
#include <device/keyboard.h>
#include <interrupt/irq.h>
#include <interrupt/idt.h>
#include <shell/shell.h>
#include <shell/shell_integration.h>
#include <util/io.h>
#include <util/init_msg.h>
#include <util/bdf.h>
#include <mem/map.h>
#include <mem/manager.h>
#include <mem/segment.h>
#include <mem/paging.h>
#include <driver/ata.h>
#include <fs/fat/fat16.h>
#include <fs/vfs.h>
#include <fs/block_cache.h>
#include <task/multi_task.h>

#ifdef UEFI_MODE
#include <driver/timer/uefi_timer.h>
#else
#include <driver/timer/apic.h>
#endif

extern struct fat16_super *g_fat16_sb;

void init_font();

void kernel_init() {
#ifdef INIT_MSG
	printk("=== KERNEL INIT ===\n");
	printk("> MEMORY INIT\n");
#endif
	memory_init();
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> PAGING64 INIT\n");
#endif
	paging64_init_kernel_pml4();
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> INTERRUPT INIT\n");
#endif

	idt_init();
	interrupt_init();
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> DEVICE INIT\n");
#endif
	keyboard_init();
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> TIMER INIT\n");
#endif

#ifdef UEFI_MODE
	// UEFI環境ではPITタイマーを使用
	int timer_result = uefi_timer_init();
	if (timer_result != 0) {
		printk("UEFI Timer initialization failed\n");
	}
	// タイマー割り込み（IRQ 0）を登録
	interrupt_register(32, uefi_timer_tick, NULL);
#else
	// レガシーBIOS環境ではAPIC Timerを使用
	interrupt_register(48, apic_timer_tick, NULL);
	int timer_result = apic_timer_init();
	if (timer_result != 0) {
		printk("APIC Timer initialization failed\n");
	}
#endif

#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> FILESYSTEM INIT (FAT16)\n");
#endif
	// ATAドライバを初期化
	if (ata_init() != 0) {
		printk("Warning: ATA initialization failed\n");
		printk("Filesystem will not be available\n");
	} else {
#ifdef INIT_MSG
		printk("ATA driver initialized\n");
#endif

		// ブロックキャッシュを初期化 (use detected ATA drive)
		int detected = ata_get_detected_drive();
		if (detected < 0)
			detected = 0; /* fallback to primary master */
		struct block_cache *cache =
			block_cache_init((uint8_t)detected, 4096, 32);
		if (cache == NULL) {
			printk("Error: Failed to initialize block cache\n");
		} else {
#ifdef INIT_MSG
			printk("Block cache initialized (32 KB, 32 entries)\n");
#endif

#ifdef INIT_MSG
			printk("Registering VFS backends\n");
#endif
			vfs_register_builtin_backends();
			// VFS を通じてファイルシステムをマウント
			if (vfs_mount_with_cache(cache) == 0) {
#ifdef INIT_MSG
				printk("FAT16 filesystem mounted successfully\n");
				printk("  Bytes/sector: %u\n",
				       g_fat16_sb->bytes_per_sector);
				printk("  Sectors/cluster: %u\n",
				       g_fat16_sb->sectors_per_cluster);
				printk("  Total sectors: %u\n",
				       g_fat16_sb->total_sectors);
#endif
			} else {
				printk("Error: Failed to mount FAT16 filesystem\n");
				block_cache_destroy(cache);
			}

			/* initialize font and then allow console to allocate gfx buffer */
			init_font();
			console_post_font_init();
		}
	}
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> MULTI TASK INIT\n");
#endif
	task_init();
#ifdef INIT_MSG
	printk("ok\n");
#endif

#ifdef INIT_MSG
	new_line();
	printk("> TSS INIT\n");
#endif
	tss_init();
#ifdef INIT_MSG
	printk("ok\n");
#endif
}

void init_font() {
	if (g_fat16_sb != NULL) {
		if (bdf_init("/kernel/fonts/ter-u12b.bdf")) {
		} else {
			printk("Warning: Failed to load BDF font\n");
		}
	} else {
		printk("Warning: Filesystem not available, skipping font loading\n");
	}
}