#include <shell/commands.h>
#include <util/console.h>
#include <mem/manager.h>
#include <mem/map.h>
#include <fs/vfs.h>
#include <driver/timer/apic.h>
#ifdef UEFI_MODE
#include <driver/timer/uefi_timer.h>
#endif
#include <device/pci.h>
#include <stdint.h>
#include <task/multi_task.h>
#include <mem/paging.h>
#include <string.h>
#include <mem/vmem.h>

// 現在のディレクトリパス（簡易実装）
static char current_path[256] = "/";

/**
 * @brief memコマンド - メモリ使用状況を表示
 */
static int cmd_mem(int argc, char **argv) {
	(void)argc;
	(void)argv;

	printk("Memory information:\n");

	const memmap_t *mm = memmap_get();
	if (!mm || mm->frames == 0) {
		printk("Physical frame map: not initialized\n");
	} else {
		uint32_t total_frames = mm->frames;
		uint32_t used_frames = 0;
		for (uint32_t i = 0; i < total_frames; ++i) {
			uint32_t word = mm->bitmap[i / 32];
			uint32_t bit = (word >> (i % 32)) & 1u;
			if (bit)
				used_frames++;
		}
		uint32_t free_frames = total_frames - used_frames;

		uint32_t total_bytes = total_frames * FRAME_SIZE;
		uint32_t used_bytes = used_frames * FRAME_SIZE;
		uint32_t free_bytes = free_frames * FRAME_SIZE;

		// 整数演算でMB表示（小数点2桁）: bytes * 100 / (1024*1024) = x.yz MB
		uint32_t total_mb_x100 = (total_bytes * 100) / (1024 * 1024);
		uint32_t used_mb_x100 = (used_bytes * 100) / (1024 * 1024);
		uint32_t free_mb_x100 = (free_bytes * 100) / (1024 * 1024);

		printk("Physical frames: total=%u (%u.%02uMB) used=%u (%u.%02uMB) free=%u (%u.%02uMB)\n",
		       total_frames, total_mb_x100 / 100, total_mb_x100 % 100,
		       used_frames, used_mb_x100 / 100, used_mb_x100 % 100,
		       free_frames, free_mb_x100 / 100, free_mb_x100 % 100);
	}

	/* Heap statistics */
	uint32_t heap_total = heap_total_bytes();
	uint32_t heap_free = heap_free_bytes();
	uint32_t heap_largest = heap_largest_free_block();

	// 整数演算でKB表示（小数点2桁）: bytes * 100 / 1024 = x.yz KB
	uint32_t total_kb_x100 = (heap_total * 100) / 1024;
	uint32_t free_kb_x100 = (heap_free * 100) / 1024;

	printk("Kernel heap: total=%u bytes (%u.%02uKB) free=%u bytes (%u.%02uKB) largest_free=%u bytes\n",
	       heap_total, total_kb_x100 / 100, total_kb_x100 % 100, heap_free,
	       free_kb_x100 / 100, free_kb_x100 % 100, heap_largest);

	return 0;
}

/**
 * @brief lsコマンド - ファイル一覧を表示
 */
static int cmd_ls(int argc, char **argv) {
	(void)argc;
	(void)argv;
	/* List current directory via VFS */
	const char *cwd = get_current_directory();
	int result = vfs_list_path(cwd);
	if (result < 0) {
		printk("Error: Failed to list directory (error=%d)\n", result);
		return -1;
	}

	return 0;
}

/**
 * @brief catコマンド - ファイルの内容を表示
 */
static int cmd_cat(int argc, char **argv) {
	if (argc < 2) {
		printk("Usage: cat <filename>\n");
		return -1;
	}

	const char *filename = argv[1];

	// 最大8KBのファイルを読み込む（スタック上）
	char buffer[8192];
	size_t bytes_read = 0;

	void *buf = NULL;
	uint32_t size = 0;
	int result = vfs_read_file_all(filename, &buf, &size);
	if (result == 0 && buf && size > 0) {
		if (size > sizeof(buffer))
			size = sizeof(buffer);
		for (uint32_t i = 0; i < size; ++i)
			buffer[i] = ((uint8_t *)buf)[i];
		bytes_read = size;
		kfree(buf);
	} else {
		printk("Error: Failed to read file '%s' (error code: %d)\n",
		       filename, result);
		return -1;
	}

	if (result != 0) {
		printk("Error: Failed to read file '%s' (error code: %d)\n",
		       filename, result);
		return -1;
	}

	if (bytes_read == 0) {
		printk("(empty file)\n");
		return 0;
	}

	// ファイル内容を表示
	for (size_t i = 0; i < bytes_read; i++) {
		printk("%c", buffer[i]);
	}

	// 改行で終わっていない場合は改行を追加
	if (buffer[bytes_read - 1] != '\n') {
		printk("\n");
	}

	return 0;
}

/**
 * @brief verコマンド - バージョン情報を表示
 */
static int cmd_ver(int argc, char **argv) {
	(void)argc;
	(void)argv;

	printk("LiteCore Kernel\n");
	printk("Version: %s\n", VERSION);
	printk("Build: %s %s\n", __DATE__, __TIME__);
	printk("Author: nekogakure\n");

	return 0;
}

/**
 * @brief uptimeコマンド - 起動時間を表示
 */
static int cmd_uptime(int argc, char **argv) {
	(void)argc;
	(void)argv;

	uint64_t uptime_ms = 0;

	if (apic_timer_available()) {
		uptime_ms = apic_get_uptime_ms();
	} else {
#ifdef UEFI_MODE
		uptime_ms = uefi_get_uptime_ms();
#else
		printk("Uptime: no timer available\n");
		return 0;
#endif
	}
	uint32_t uptime_ms_low = (uint32_t)uptime_ms; /* 下位32bitを取得 */
	uint32_t total_seconds = uptime_ms_low / 1000UL; /* ミリ秒を秒に変換 */

	uint32_t days = total_seconds / 86400UL;
	uint32_t hours = (total_seconds % 86400UL) / 3600UL;
	uint32_t minutes = (total_seconds % 3600UL) / 60UL;
	uint32_t seconds = total_seconds % 60UL;

	printk("System uptime: ");
	if (days > 0) {
		printk("%u days, ", days);
	}
	printk("%02u:%02u:%02u\n", hours, minutes, seconds);

	return 0;
}

static int cmd_change_dir(int argc, char **argv) {
	if (argc < 2) {
		printk("Usage: cd <directory>\n");
		return -1;
	}
	const char *path = argv[1];
	/* Build absolute path from current_path and provided path, normalize '.' and '..' */
	char tmp[256];
	/* If path is absolute, start from it */
	if (path[0] == '/') {
		/* copy path */
		int i = 0;
		for (; i < (int)sizeof(tmp) - 1 && path[i]; ++i)
			tmp[i] = path[i];
		tmp[i] = '\0';
	} else {
		/* relative: join current_path and path */
		int i = 0;
		/* copy current_path (without trailing slash except root) */
		if (current_path[0] == '/' && current_path[1] == '\0') {
			tmp[i++] = '/';
		} else {
			for (int j = 0;
			     current_path[j] && i < (int)sizeof(tmp) - 1; ++j)
				tmp[i++] = current_path[j];
		}
		if (i > 0 && tmp[i - 1] != '/' && tmp[0] != '\0') {
			if (i < (int)sizeof(tmp) - 1)
				tmp[i++] = '/';
		}
		for (int j = 0; path[j] && i < (int)sizeof(tmp) - 1; ++j)
			tmp[i++] = path[j];
		tmp[i] = '\0';
	}

	/* normalize components */
	char comps[64][64];
	int comp_count = 0;
	int p = 0;
	int len = 0;
	while (tmp[p]) {
		/* skip slashes */
		while (tmp[p] == '/')
			p++;
		if (!tmp[p])
			break;
		len = 0;
		while (tmp[p] && tmp[p] != '/' && len < 63) {
			comps[comp_count][len++] = tmp[p++];
		}
		comps[comp_count][len] = '\0';
		if (len == 0)
			break;
		if (comps[comp_count][0] == '.' &&
		    comps[comp_count][1] == '\0') {
			/* ignore '.' */
		} else if (comps[comp_count][0] == '.' &&
			   comps[comp_count][1] == '.' &&
			   comps[comp_count][2] == '\0') {
			if (comp_count > 0)
				comp_count--; /* pop previous */
		} else {
			/* keep component */
			comp_count++;
			if (comp_count >= 64)
				break;
		}
	}

	/* rebuild normalized path */
	char newpath[256];
	int idx = 0;
	if (comp_count == 0) {
		newpath[0] = '/';
		newpath[1] = '\0';
	} else {
		for (int i = 0; i < comp_count; ++i) {
			if (idx < (int)sizeof(newpath) - 1)
				newpath[idx++] = '/';
			int j = 0;
			while (comps[i][j] && idx < (int)sizeof(newpath) - 1)
				newpath[idx++] = comps[i][j++];
		}
		newpath[idx] = '\0';
	}

	/* Verify target exists and is a directory */
	int is_dir = 0;
	uint32_t size = 0;
	int r = vfs_resolve_path(newpath, &is_dir, &size);
	if (r != 0) {
		printk("cd: path not found: %s\n", newpath);
		return -1;
	}
	if (!is_dir) {
		printk("cd: not a directory: %s\n", newpath);
		return -1;
	}

	for (int i = 0; i < (int)sizeof(current_path); ++i)
		current_path[i] = '\0';
	for (int i = 0; i < (int)sizeof(current_path) - 1 && newpath[i]; ++i)
		current_path[i] = newpath[i];
	/* ensure null termination */
	current_path[(int)sizeof(current_path) - 1] = '\0';
	return 0;
}

/**
 * @brief pwdコマンド - 現在のディレクトリを表示
 */
static int cmd_pwd(int argc, char **argv) {
	(void)argc;
	(void)argv;

	printk("%s\n", current_path);
	return 0;
}

/**
 * @brief 現在のディレクトリパスを取得
 */
const char *get_current_directory(void) {
	return current_path;
}

/**
 * @brief PCIクラスコードから説明文字列を取得
 */
static const char *pci_get_class_name(uint8_t base_class, uint8_t sub_class) {
	switch (base_class) {
	case 0x00:
		return "Unclassified";
	case 0x01:
		switch (sub_class) {
		case 0x01:
			return "IDE Controller";
		case 0x05:
			return "ATA Controller";
		case 0x06:
			return "SATA Controller";
		case 0x08:
			return "NVME Controller";
		default:
			return "Mass Storage Controller";
		}
	case 0x02:
		return "Network Controller";
	case 0x03:
		switch (sub_class) {
		case 0x00:
			return "VGA Controller";
		case 0x01:
			return "XGA Controller";
		default:
			return "Display Controller";
		}
	case 0x04:
		return "Multimedia Controller";
	case 0x05:
		return "Memory Controller";
	case 0x06:
		switch (sub_class) {
		case 0x00:
			return "Host Bridge";
		case 0x01:
			return "ISA Bridge";
		case 0x04:
			return "PCI-to-PCI Bridge";
		default:
			return "Bridge Device";
		}
	case 0x07:
		return "Communication Controller";
	case 0x08:
		return "System Peripheral";
	case 0x09:
		return "Input Device";
	case 0x0A:
		return "Docking Station";
	case 0x0B:
		return "Processor";
	case 0x0C:
		switch (sub_class) {
		case 0x00:
			return "FireWire Controller";
		case 0x03:
			return "USB Controller";
		default:
			return "Serial Bus Controller";
		}
	case 0x0D:
		return "Wireless Controller";
	case 0x0E:
		return "Intelligent I/O Controller";
	case 0x0F:
		return "Satellite Controller";
	case 0x10:
		return "Encryption/Decryption Controller";
	case 0x11:
		return "Data Acquisition Controller";
	default:
		return "Unknown Device";
	}
}

/**
 * @brief ベンダーIDから名前を取得（主要なベンダーのみ）
 */
static const char *pci_get_vendor_name(uint16_t vendor_id) {
	switch (vendor_id) {
	case 0x8086:
		return "Intel";
	case 0x1234:
		return "QEMU";
	case 0x1b36:
		return "Red Hat";
	case 0x1022:
		return "AMD";
	case 0x10de:
		return "NVIDIA";
	case 0x1002:
		return "ATI/AMD";
	default:
		return "Unknown";
	}
}

/**
 * @brief devicesコマンド - 接続されているデバイス一覧を表示
 */
static int cmd_devices(int argc, char **argv) {
	int verbose = 0;

	// -v オプションで詳細表示
	if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'v') {
		verbose = 1;
	}

	printk("Scanning PCI devices...\n");
	printk("================================================================================\n");
	if (verbose) {
		printk("Bus:Dev.Fn  Vendor:Device  Class  Description\n");
	} else {
		printk("Bus  Dev  Func  Vendor  Device  Class  Description\n");
	}
	printk("================================================================================\n");

	int device_count = 0;

	for (uint16_t bus = 0; bus < 256; ++bus) {
		for (uint8_t device = 0; device < 32; ++device) {
			for (uint8_t func = 0; func < 8; ++func) {
				uint32_t data = pci_read_config_dword(
					(uint8_t)bus, device, func, 0);
				uint16_t vendor = (uint16_t)(data & 0xFFFF);
				if (vendor == 0xFFFF) {
					continue; // デバイスなし
				}

				uint16_t device_id =
					(uint16_t)((data >> 16) & 0xFFFF);
				uint32_t class_rev = pci_read_config_dword(
					(uint8_t)bus, device, func, 0x08);
				uint8_t base_class = (class_rev >> 24) & 0xFF;
				uint8_t sub_class = (class_rev >> 16) & 0xFF;

				const char *class_name = pci_get_class_name(
					base_class, sub_class);

				if (verbose) {
					const char *vendor_name =
						pci_get_vendor_name(vendor);
					printk("%02x:%02x.%x     %s [%04x:%04x]  0x%02x   %s\n",
					       bus, device, func, vendor_name,
					       vendor, device_id, base_class,
					       class_name);
				} else {
					printk("%3d  %3d  %4d  0x%04x  0x%04x  0x%02x   %s\n",
					       bus, device, func, vendor,
					       device_id, base_class,
					       class_name);
				}

				device_count++;

				// マルチファンクションデバイスでなければ funcループを抜ける
				if (func == 0) {
					uint32_t hdr0 = pci_read_config_dword(
						(uint8_t)bus, device, func,
						0x0C);
					if (((hdr0 >> 16) & 0x80) == 0) {
						break; // single function device
					}
				}
			}
		}
	}

	printk("================================================================================\n");
	printk("Total devices found: %d\n", device_count);
	if (!verbose) {
		printk("Tip: Use 'devices -v' for verbose output\n");
	}

	return 0;
}

/* Minimal ELF loader for ELF64 program headers -> map PT_LOAD into new task PD */
typedef unsigned long Elf64_Addr;
typedef unsigned long Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

typedef struct {
	unsigned char e_ident[16];
	Elf64_Half e_type;
	Elf64_Half e_machine;
	Elf64_Word e_version;
	Elf64_Addr e_entry;
	Elf64_Off e_phoff;
	Elf64_Off e_shoff;
	Elf64_Word e_flags;
	Elf64_Half e_ehsize;
	Elf64_Half e_phentsize;
	Elf64_Half e_phnum;
	Elf64_Half e_shentsize;
	Elf64_Half e_shnum;
	Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	Elf64_Word p_type;
	Elf64_Word p_flags;
	Elf64_Off p_offset;
	Elf64_Addr p_vaddr;
	Elf64_Addr p_paddr;
	Elf64_Xword p_filesz;
	Elf64_Xword p_memsz;
	Elf64_Xword p_align;
} Elf64_Phdr;

/* runコマンド - ELF をロードしてユーザータスクを作る */
static int cmd_run(int argc, char **argv) {
	if (argc < 2) {
		printk("Usage: run <path-to-elf>\n");
		return -1;
	}
	const char *path = argv[1];
	void *buf = NULL;
	uint32_t size = 0;
	int r = vfs_read_file_all(path, &buf, &size);
	if (r != 0 || !buf || size < sizeof(Elf64_Ehdr)) {
		printk("run: failed to read '%s' (err=%d)\n", path, r);
		if (buf)
			kfree(buf);
		return -1;
	}

	Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
	if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
		printk("run: not an ELF: %s\n", path);
		kfree(buf);
		return -1;
	}

	/* create user-mode task but do NOT ready it until mapping is done */
	task_t *t = task_create((void *)0x0, path, 0);
	if (!t) {
		printk("run: task_create failed\n");
		kfree(buf);
		return -1;
	}

	uint32_t pd_phys = (uint32_t)t->page_directory;
	if (pd_phys == 0) {
		printk("run: invalid page directory for task\n");
		kfree(buf);
		return -1;
	}

	/* parse program headers and map PT_LOAD segments */
	Elf64_Phdr *ph = (Elf64_Phdr *)((uint8_t *)buf + eh->e_phoff);
	for (int i = 0; i < eh->e_phnum; ++i) {
		if (ph[i].p_type != 1) /* PT_LOAD */
			continue;

		Elf64_Addr vaddr = (Elf64_Addr)ph[i].p_vaddr;
		uint64_t filesz = (uint64_t)ph[i].p_filesz;
		uint64_t memsz = (uint64_t)ph[i].p_memsz;
		uint64_t off = (uint64_t)ph[i].p_offset;

		/* round down vaddr to page, handle offset inside page */
		uint32_t page_off = (uint32_t)(vaddr & 0xFFF);
		uint32_t map_start = (uint32_t)(vaddr & ~0xFFFUL);
		uint32_t to_map =
			(uint32_t)(((page_off + memsz) + 0xFFF) & ~0xFFFUL);

		uint32_t pages = to_map / 0x1000;
		for (uint32_t p = 0; p < pages; ++p) {
			void *frame = alloc_frame();
			if (!frame) {
				printk("run: alloc_frame failed for segment\n");
				kfree(buf);
				return -1;
			}
			uint32_t phys = (uint32_t)(uintptr_t)frame;
			uint32_t dest_virt = map_start + p * 0x1000;

			/* map the page into the new page directory */
			int mf = map_page_pd(pd_phys, phys, dest_virt,
					     PAGING_PRESENT | PAGING_RW |
						     PAGING_USER);
			if (mf != 0) {
				printk("run: map_page_pd failed for va=0x%x\n",
				       dest_virt);
				kfree(buf);
				return -1;
			}

			/* copy file data into mapped physical frame (via virt) */
			uint32_t frame_virt = vmem_phys_to_virt(phys);
			if (frame_virt == 0) {
				printk("run: vmem_phys_to_virt failed for phys=0x%x\n",
				       phys);
				kfree(buf);
				return -1;
			}
			uint8_t *dst = (uint8_t *)(uintptr_t)frame_virt;
			/* calculate portion of file that maps into this page */
			uint32_t page_va = dest_virt;
			uint64_t copy_from;
			if (page_va + 0x1000 <= vaddr) {
				/* page before file data start */
				copy_from = 0;
			}
			/* For simplicity, copy relevant bytes: */
			uint64_t seg_file_end = off + filesz;
			uint64_t src_off;
			if ((uint64_t)page_va + 0x1000 <= vaddr) {
				src_off = 0; /* nothing from file */
			} else {
				/* overlapping region */
				uint64_t file_page_offset = 0;
				if (vaddr > (uint64_t)page_va)
					file_page_offset = 0;
				else
					file_page_offset =
						(uint64_t)(page_va - vaddr);
				uint64_t src = off + file_page_offset;
				uint64_t max_copy = 0;
				if (src < seg_file_end) {
					max_copy = seg_file_end - src;
					if (max_copy > 0x1000)
						max_copy = 0x1000;
					/* copy from buf[src .. src+max_copy) to dst[offset_in_page] */
					uint32_t dst_off =
						(uint32_t)((vaddr > page_va) ?
								   (vaddr -
								    page_va) :
								   0);
					uint8_t *srcp =
						(uint8_t *)buf + (size_t)src;
					for (uint32_t z = 0;
					     z < (uint32_t)max_copy; ++z)
						dst[dst_off + z] = srcp[z];
					/* zero the rest */
					for (uint32_t z = dst_off +
							  (uint32_t)max_copy;
					     z < 0x1000; ++z)
						dst[z] = 0;
				} else {
					/* entirely zero page */
					for (uint32_t z = 0; z < 0x1000; ++z)
						dst[z] = 0;
				}
			}
		}
	}

	/* set entry point (truncate to 32-bit virtual address) */
	uint32_t entry = (uint32_t)(eh->e_entry & 0xFFFFFFFFu);
	t->regs.rip = (uint64_t)entry;

	/* mark task ready */
	task_ready(t);
	kfree(buf);
	printk("run: started '%s' (TID=%u) entry=0x%x\n", path,
	       (unsigned)t->tid, entry);
	return 0;
}

/**
 * @brief 拡張コマンドを登録
 */
void register_extended_commands(void) {
	register_command("mem", "Display memory information", cmd_mem);
	register_command("ls", "List directory contents", cmd_ls);
	register_command("cat", "Display file contents", cmd_cat);
	register_command("ver", "Display version information", cmd_ver);
	register_command("uptime", "Display system uptime", cmd_uptime);
	register_command("cd", "Change directory", cmd_change_dir);
	register_command("pwd", "Print working directory", cmd_pwd);
	register_command("devices", "List connected devices", cmd_devices);
	register_command("run", "Run user ELF: run <path>", cmd_run);
}
