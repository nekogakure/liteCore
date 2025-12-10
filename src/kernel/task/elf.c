#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <util/console.h>
#include <mem/manager.h>
#include <mem/paging.h>
#include <mem/vmem.h>
#include <mem/map.h>
#include <mem/tss.h>
#include <task/multi_task.h>
#include <task/elf.h>
#include <fs/vfs.h>

// VFS定義
#ifndef O_RDONLY
#define O_RDONLY 0
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

// ssize_t定義
typedef long ssize_t;

// セグメントのロード情報を追跡
static void *loaded_base = NULL; // 最初のPT_LOADセグメントの実際のアドレス
static uint64_t loaded_vaddr = 0; // 最初のPT_LOADセグメントの期待vaddr

// ELF64 ヘッダー定義
#define ELF_MAGIC 0x464C457F // "\x7FELF"
#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_TYPE_EXEC 2
#define ELF_MACHINE_X86_64 0x3E

#define PT_LOAD 1
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
	uint32_t magic;
	uint8_t class;
	uint8_t data;
	uint8_t version;
	uint8_t osabi;
	uint8_t abiversion;
	uint8_t pad[7];
	uint16_t type;
	uint16_t machine;
	uint32_t version2;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} __attribute__((packed)) elf64_header_t;

typedef struct {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
} __attribute__((packed)) elf64_program_header_t;

/**
 * @brief ELFヘッダーを検証
 */
static int validate_elf_header(const elf64_header_t *header) {
	if (header->magic != ELF_MAGIC) {
		printk("ELF: Invalid magic number\n");
		return -1;
	}
	if (header->class != ELF_CLASS_64) {
		printk("ELF: Not a 64-bit ELF\n");
		return -1;
	}
	if (header->data != ELF_DATA_LSB) {
		printk("ELF: Not little-endian\n");
		return -1;
	}
	if (header->type != ELF_TYPE_EXEC) {
		printk("ELF: Not an executable\n");
		return -1;
	}
	if (header->machine != ELF_MACHINE_X86_64) {
		printk("ELF: Not x86-64\n");
		return -1;
	}
	return 0;
}

/**
 * @brief プログラムセグメントをメモリにロード
 */
static int load_segment(int fd, const elf64_program_header_t *ph,
			uint64_t pd_phys) {
	if (ph->type != PT_LOAD) {
		return 0; // ロード不要なセグメント
	}

	// メモリのアライメント調整
	uint64_t vaddr_base = ph->vaddr & ~0xFFF; // 4KBページ境界に整列
	uint64_t vaddr_offset = ph->vaddr & 0xFFF;
	uint64_t total_size = vaddr_offset + ph->memsz;
	uint64_t pages_needed = (total_size + 0xFFF) >> 12;

	// ページフラグを設定
	uint32_t flags = PAGING_PRESENT | PAGING_USER;
	if (ph->flags & PF_W) {
		flags |= PAGING_RW;
	}

	// 割り当てたページを追跡する配列（ポインタではなく物理アドレスを保存）
	uint32_t *page_phys_addrs = kmalloc(pages_needed * sizeof(uint32_t));
	if (!page_phys_addrs) {
		printk("ELF: Failed to allocate page tracking array\n");
		return -1;
	}

	// メモリを割り当ててマップ
	for (uint64_t i = 0; i < pages_needed; i++) {
		// alloc_frame()を使用してページを割り当て
		void *page_phys = alloc_frame();
		if (!page_phys) {
			printk("ELF: Failed to allocate page frame\n");
			// クリーンアップ
			kfree(page_phys_addrs);
			return -1;
		}

		// 物理アドレスを保存
		page_phys_addrs[i] = (uint32_t)(uintptr_t)page_phys;

		// カーネル仮想アドレスに変換してゼロクリア
		uint32_t page_virt =
			vmem_phys_to_virt((uint32_t)(uintptr_t)page_phys);
		memset((void *)(uintptr_t)page_virt, 0, 4096);

		uint64_t vaddr = vaddr_base + (i << 12);

		// 64ビットページング関数を使用（タスクのPML4に直接マップ）
		if (map_page_64(pd_phys, (uint64_t)(uintptr_t)page_phys, vaddr,
				flags) != 0) {
			printk("ELF: Failed to map page at 0x%lx\n", vaddr);
			kfree(page_phys_addrs);
			return -1;
		}
		printk("ELF: Mapped vaddr=0x%lx -> phys=0x%lx\n", vaddr,
		       (uint64_t)(uintptr_t)page_phys);
	}

	// ファイルからデータを読み込む（チャンク単位で直接ページに読み込む）
	if (ph->filesz > 0) {
		vfs_lseek(fd, ph->offset, SEEK_SET);

		// データをページに直接読み込む（大きなバッファ割り当てを避ける）
		uint64_t copied = 0;
		for (uint64_t i = 0; i < pages_needed && copied < ph->filesz;
		     i++) {
			uint64_t offset = (i == 0) ? vaddr_offset : 0;
			uint64_t copy_size =
				(ph->filesz - copied > 4096 - offset) ?
					(4096 - offset) :
					(ph->filesz - copied);

			// 物理アドレスをカーネル仮想アドレスに変換
			uint32_t page_virt =
				vmem_phys_to_virt(page_phys_addrs[i]);

			// ファイルから直接ページに読み込む
			ssize_t read_bytes = vfs_read(
				fd, (uint8_t *)(uintptr_t)page_virt + offset,
				copy_size);
			if (read_bytes != (ssize_t)copy_size) {
				printk("ELF: Failed to read segment data\n");
				kfree(page_phys_addrs);
				return -1;
			}
			copied += copy_size;
		}
	}

	kfree(page_phys_addrs);
	printk("ELF: Loaded segment vaddr=0x%lx-0x%lx flags=%c%c%c\n",
	       ph->vaddr, ph->vaddr + ph->memsz, (ph->flags & PF_R) ? 'R' : '-',
	       (ph->flags & PF_W) ? 'W' : '-', (ph->flags & PF_X) ? 'X' : '-');
	return 0;
}

/**
 * @brief ELFタスクのダミーエントリポイント
 * 実際のエントリポイントはレジスタで上書きされる
 */
static void elf_dummy_entry(void) {
	// このコードは実行されない
	// 実際のエントリポイントはregs.ripで設定される
	while (1) {
		asm volatile("hlt");
	}
}

/**
 * @brief ELFファイルを実行
 */
int elf_run(const char *path) {
	printk("ELF: Loading %s\n", path);

	// ファイルを開く
	int fd = vfs_open(path, O_RDONLY, 0);
	if (fd < 0) {
		printk("ELF: Failed to open %s\n", path);
		return -1;
	}

	// ELFヘッダーを読み込む
	elf64_header_t header;
	ssize_t read_size = vfs_read(fd, &header, sizeof(header));
	if (read_size != sizeof(header)) {
		printk("ELF: Failed to read header\n");
		vfs_close(fd);
		return -1;
	}

	// ヘッダーを検証
	if (validate_elf_header(&header) != 0) {
		vfs_close(fd);
		return -1;
	}

	printk("ELF: Entry point: 0x%x\n", (uint32_t)header.entry);
	printk("ELF: Program headers: %d\n", header.phnum);

	// まずタスクを作成（ページディレクトリとスタックを確保）
	task_t *new_task = task_create(elf_dummy_entry, path, 0);
	if (!new_task) {
		printk("ELF: Failed to create task\n");
		vfs_close(fd);
		return -1;
	}

	// タスク情報を保存（ポインタは破壊される可能性があるため値をコピー）
	uint32_t tid = new_task->tid;
	uint64_t pd_phys = new_task->page_directory;
	uint64_t user_stack_phys = new_task->user_stack;

	// ユーザースタックをユーザー空間の仮想アドレスにマップ
	// 注意: タスクの新しいPML4にマップする必要があるため、map_page_64()を直接使用
	uint64_t user_stack_virt = 0x7FFFE000ULL; // 2GB - 8KB
	printk("ELF: Mapping user stack phys=0x%lx to virt=0x%lx in PML4=0x%lx\n",
	       user_stack_phys, user_stack_virt, pd_phys);
	if (map_page_64(pd_phys, user_stack_phys, user_stack_virt,
			PAGING_RW | PAGING_USER | PAGING_PRESENT) != 0) {
		printk("ELF: Failed to map user stack at 0x%lx\n",
		       user_stack_virt);
		vfs_close(fd);
		return -1;
	}
	printk("ELF: User stack mapped successfully\n");
	// スタックトップに設定（下向きに成長するため、ページの最上位アドレス）
	uint64_t user_stack_top = user_stack_virt + 0x1000;

	printk("ELF: Task created - TID=%u, PD=0x%lx, user_stack=0x%lx\n", tid,
	       pd_phys, user_stack_top);

	// プログラムヘッダーを読み込んでセグメントをロード
	// 既存のページディレクトリにマップする
	for (uint16_t i = 0; i < header.phnum; i++) {
		elf64_program_header_t ph;
		vfs_lseek(fd, header.phoff + i * sizeof(elf64_program_header_t),
			  SEEK_SET);

		if (vfs_read(fd, &ph, sizeof(ph)) != sizeof(ph)) {
			printk("ELF: Failed to read program header %d\n", i);
			vfs_close(fd);
			// TODO: タスクをクリーンアップ
			return -1;
		}

		if (load_segment(fd, &ph, pd_phys) != 0) {
			printk("ELF: Failed to load segment %d\n", i);
			vfs_close(fd);
			// TODO: タスクをクリーンアップ
			return -1;
		}
	}

	vfs_close(fd);

	// デバッグ: CR3の確認
	uint64_t current_cr3;
	asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
	printk("ELF: Current CR3=0x%lx, will switch to CR3=0x%lx\n",
	       current_cr3, pd_phys);

	// デバッグ: エントリポイントの確認
	printk("ELF: Entry point=0x%lx is in segment 0x401000-0x40b06e (R-X)\n",
	       header.entry);

	// TSSにカーネルスタックを設定
	// ユーザーモードから例外/割り込みが発生した時のスタックポインタ
	uint64_t kernel_stack;
	asm volatile("mov %%rsp, %0" : "=r"(kernel_stack));
	tss_set_kernel_stack(kernel_stack);
	printk("ELF: Set TSS kernel stack to 0x%lx\n", kernel_stack);

	// ユーザーモードタスクを起動
	printk("ELF: Launching usermode at entry=0x%lx stack=0x%lx CR3=0x%lx\n",
	       header.entry, user_stack_top, pd_phys);
	printk("ELF: About to call task_enter_usermode(entry=0x%lx, stack=0x%lx, cr3=0x%lx)\n",
	       header.entry, user_stack_top, pd_phys);

	// デバッグ: header.entryの実際の値を16進数で表示
	printk("ELF: header.entry raw value = 0x%016lx\n", header.entry);
	printk("ELF: user_stack_top raw value = 0x%016lx\n", user_stack_top);
	printk("ELF: pd_phys raw value = 0x%016lx\n", pd_phys);

	// デバッグ: 現在のスタックポインタを表示
	uint64_t current_rsp;
	asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
	printk("ELF: Current RSP before task_enter_usermode = 0x%lx\n",
	       current_rsp);

	// デバッグ: スタック上の内容を表示
	uint64_t *stack_ptr = (uint64_t *)current_rsp;
	printk("ELF: Stack contents before call:\n");
	for (int i = -2; i < 6; i++) {
		printk("  [RSP%+d] = 0x%016lx\n", i * 8, stack_ptr[i]);
	}

	task_enter_usermode(header.entry, user_stack_top, pd_phys);

	// ここには戻ってこない
	printk("ELF: ERROR - returned from usermode!\n");

	return 0;
}