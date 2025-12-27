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

#if 0 // デバッグ用 - 必要に応じて有効化
static uint64_t resolve_phys_in_pml4(uint64_t pml4_phys, uint64_t virt) {
	/* indices */
	uint64_t pml4_idx = (virt >> 39) & 0x1FFULL;
	uint64_t pdpt_idx = (virt >> 30) & 0x1FFULL;
	uint64_t pd_idx = (virt >> 21) & 0x1FFULL;
	uint64_t pt_idx = (virt >> 12) & 0x1FFULL;
	uint64_t page_off = virt & 0xFFFULL;

	uint64_t pml4_virt = vmem_phys_to_virt64(pml4_phys);
	if (pml4_virt == UINT64_MAX)
		return UINT64_MAX;
	uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_virt;
	uint64_t entry = pml4[pml4_idx];
	if ((entry & 0x1) == 0)
		return UINT64_MAX;

	uint64_t pdpt_phys = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pdpt_virt = vmem_phys_to_virt64(pdpt_phys);
	if (pdpt_virt == UINT64_MAX)
		return UINT64_MAX;
	uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_virt;
	entry = pdpt[pdpt_idx];
	if ((entry & 0x1) == 0)
		return UINT64_MAX;
	if (entry & (1ULL << 7)) {
		uint64_t base = entry & 0xFFFFFC0000000ULL;
		uint64_t offset = virt & 0x3FFFFFFFULL;
		return base + offset;
	}

	uint64_t pd_phys = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pd_virt = vmem_phys_to_virt64(pd_phys);
	if (pd_virt == UINT64_MAX)
		return UINT64_MAX;
	uint64_t *pd = (uint64_t *)(uintptr_t)pd_virt;
	entry = pd[pd_idx];
	if ((entry & 0x1) == 0)
		return UINT64_MAX;
	if (entry & (1ULL << 7)) {
		uint64_t base = entry & 0xFFFFFFFFFFE00000ULL;
		uint64_t offset = virt & 0x1FFFFFULL;
		return base + offset;
	}

	uint64_t pt_phys = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pt_virt = vmem_phys_to_virt64(pt_phys);
	if (pt_virt == UINT64_MAX)
		return UINT64_MAX;
	uint64_t *pt = (uint64_t *)(uintptr_t)pt_virt;
	entry = pt[pt_idx];
	if ((entry & 0x1) == 0)
		return UINT64_MAX;

	uint64_t page_base = entry & 0xFFFFFFFFFFFFF000ULL;
	return page_base + page_off;
}

static void debug_dump_entry_bytes(uint64_t pd_phys, uint64_t entry) {
	uint64_t phys = resolve_phys_in_pml4(pd_phys, entry);
	if (phys == UINT64_MAX) {
		printk("ELF-DUMP: could not resolve entry 0x%lx in PML4=0x%lx\n",
		       entry, pd_phys);
		return;
	}
	uint64_t kvirt = vmem_phys_to_virt64(phys & 0xFFFFFFFFFFFFF000ULL);
	if (kvirt == UINT64_MAX) {
		printk("ELF-DUMP: vmem_phys_to_virt64 failed for phys=0x%lx\n",
		       phys);
		return;
	}
	uint8_t *p = (uint8_t *)(uintptr_t)(kvirt + (phys & 0xFFFULL));
	printk("ELF-DUMP: entry phys=0x%lx kvirt=0x%lx offset=0x%lx bytes:",
	       phys, kvirt, (unsigned long)(phys & 0xFFFULL));
	for (int i = 0; i < 16; i++) {
		printk(" %02x", (unsigned)p[i]);
	}
	printk("\n");
}

/*
 * Debug helper called from task_switch.asm before iretq to print
 * the values that will be used for the user-mode transition.
 */
void debug_print_iretq_frame(uint64_t rip, uint64_t rsp, uint64_t cr3) {
	uint64_t rflags;
	asm volatile("pushfq; pop %0" : "=r"(rflags));
	rflags |= 0x200; // IF=1

	printk("IRETQ-FRAME: RIP=0x%lx CS=0x2B RFLAGS=0x%lx RSP=0x%lx SS=0x23 CR3=0x%lx\n",
	       rip, rflags, rsp, cr3);

	// Dump GDT to verify segment descriptors
	extern void gdt_dump(void);
	gdt_dump();

	// Verify that the user stack page is actually mapped
	uint64_t stack_phys = resolve_phys_in_pml4(cr3, rsp);
	if (stack_phys == UINT64_MAX) {
		printk("ERROR: User stack RSP=0x%lx is NOT mapped in CR3=0x%lx!\n",
		       rsp, cr3);
	} else {
		printk("User stack RSP=0x%lx resolves to phys=0x%lx (OK)\n",
		       rsp, stack_phys);
	}

	// Verify RIP is mapped
	uint64_t rip_phys = resolve_phys_in_pml4(cr3, rip);
	if (rip_phys == UINT64_MAX) {
		printk("ERROR: Entry RIP=0x%lx is NOT mapped in CR3=0x%lx!\n",
		       rip, cr3);
	} else {
		printk("Entry RIP=0x%lx resolves to phys=0x%lx (OK)\n", rip,
		       rip_phys);
	}
}
#endif

// デバッグ用スナップショット変数: task_enter_usermode 呼び出し直前のレジスタ/スタック
volatile uint64_t elf_call_snapshot_func_addr = 0;
volatile uint64_t elf_call_snapshot_rdi = 0;
volatile uint64_t elf_call_snapshot_rsi = 0;
volatile uint64_t elf_call_snapshot_rdx = 0;
volatile uint64_t elf_call_snapshot_rsp = 0;

// VFS定義
#ifndef O_RDONLY
#define O_RDONLY 0
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

// ssize_t定義
typedef long ssize_t;

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

/// @brief ELF64ヘッダ構造体
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

	// ファイルからデータを読み込む準備
	if (ph->filesz > 0) {
		vfs_lseek(fd, ph->offset, SEEK_SET);
	}

	// メモリを割り当ててマップ（ページごとに処理）
	uint64_t copied = 0;
	for (uint64_t i = 0; i < pages_needed; i++) {
		// alloc_frame()を使用してページを割り当て
		void *page_phys = alloc_frame();
		if (!page_phys) {
			printk("ELF: Failed to allocate page frame\n");
			return -1;
		}

		// カーネル仮想アドレスに変換してゼロクリア
		uint32_t page_virt =
			vmem_phys_to_virt((uint32_t)(uintptr_t)page_phys);

		memset((void *)(uintptr_t)page_virt, 0, 4096);

		// このページにファイルデータを読み込む（該当する場合）
		if (ph->filesz > 0 && copied < ph->filesz) {
			uint64_t offset = (i == 0) ? vaddr_offset : 0;
			uint64_t copy_size =
				(ph->filesz - copied > 4096 - offset) ?
					(4096 - offset) :
					(ph->filesz - copied);

			// ファイルから直接ページに読み込む
			ssize_t read_bytes = vfs_read(
				fd, (uint8_t *)(uintptr_t)page_virt + offset,
				copy_size);
			if (read_bytes != (ssize_t)copy_size) {
				printk("ELF: Failed to read segment data\n");
				return -1;
			}
			copied += copy_size;
		}

		uint64_t vaddr = vaddr_base + (i << 12);

		// 64ビットページング関数を使用（タスクのPML4に直接マップ）
		if (map_page_64(pd_phys, (uint64_t)(uintptr_t)page_phys, vaddr,
				flags) != 0) {
			printk("ELF: Failed to map page at 0x%lx\n", vaddr);
			return -1;
		}
	}
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

	// まずタスクを作成（ページディレクトリとスタックを確保）
	task_t *new_task = task_create(elf_dummy_entry, path, 0);
	if (!new_task) {
		printk("ELF: Failed to create task\n");
		vfs_close(fd);
		return -1;
	}

	// タスク情報を保存（ポインタは破壊される可能性があるため値をコピー）
	uint64_t pd_phys = new_task->page_directory;
	uint64_t user_stack_phys = new_task->user_stack;

// ユーザースタックをユーザー空間の仮想アドレスにマップ
// USER_STACK_SIZE (16KB = 4 pages) を全てマップする
#define USER_STACK_SIZE 0x4000 // 16KB (from multi_task.c)
#define PAGE_SIZE_4K 0x1000
	uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE_4K; // 4 pages
	uint64_t user_stack_base =
		0x7FFFB000ULL; // Start of stack area (2GB - 20KB)

	// Map all stack pages
	for (uint64_t i = 0; i < stack_pages; i++) {
		uint64_t virt_addr = user_stack_base + (i * PAGE_SIZE_4K);
		uint64_t phys_addr = user_stack_phys + (i * PAGE_SIZE_4K);
		if (map_page_64(pd_phys, phys_addr, virt_addr,
				PAGING_RW | PAGING_USER | PAGING_PRESENT) !=
		    0) {
			printk("ELF: Failed to map user stack page %llu at 0x%lx\n",
			       (unsigned long long)i, virt_addr);
			vfs_close(fd);
			return -1;
		}
	}

	// スタックトップに設定（下向きに成長するため、マップした領域の最上位アドレス）
	// Note: Normally x86-64 ABI expects RSP to be (16n + 8) before a call instruction,
	// but newlib's _start and initialization code appear to expect RSP to be 16-byte aligned (16n).
	// This may be due to how newlib was compiled or CRT setup requirements.
	uint64_t user_stack_top = (user_stack_base + USER_STACK_SIZE) & ~0xFULL;

	// Setup initial stack contents (argc = 0) using kernel mapping
	// Convert user stack physical address to kernel virtual address
	uint64_t kernel_stack_virt = vmem_phys_to_virt64(user_stack_phys);
	if (kernel_stack_virt == UINT64_MAX) {
		printk("ELF: Failed to map user stack to kernel virtual address\n");
		vfs_close(fd);
		return -1;
	}
	// Write argc=0 at the top of stack (RSP will point here)
	// Stack is 16KB, so the top is at offset (USER_STACK_SIZE - 8)
	uint64_t *stack_ptr = (uint64_t *)(uintptr_t)(kernel_stack_virt +
						      USER_STACK_SIZE - 8);
	*stack_ptr = 0; // argc = 0
	// user_stack_top stays at 16-byte boundary, pointing just above argc
	// (no need to subtract 8, keeping it at 16n for newlib compatibility)

	// プログラムヘッダーを読み込んでセグメントをロード
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

	// TSSにカーネルスタックを設定
	// ユーザーモードから例外/割り込みが発生した時のスタックポインタ
	uint64_t kernel_stack;
	asm volatile("mov %%rsp, %0" : "=r"(kernel_stack));
	tss_set_kernel_stack(kernel_stack);

	/*
		 * inline asm で以下を行う:
		 *  - 関数ポインタを RAX に読み込む
		 *  - 引数を RDI/RSI/RDX に設定
		 *  - RAX/RDI/RSI/RDX/RSP の値を elf_call_snapshot に書き出す
		 *  - RAX 経由でコールを実行
		 */
	void (*fn)(uint64_t, uint64_t, uint64_t) = task_enter_usermode;
	uint64_t fnaddr = (uint64_t)fn;

	asm volatile("mov %[fnaddr], %%rax\n\t"
		     "mov %[a1], %%rdi\n\t"
		     "mov %[a2], %%rsi\n\t"
		     "mov %[a3], %%rdx\n\t"
		     /* store registers into the snapshot memory */
		     "mov %%rax, %[out_func]\n\t"
		     "mov %%rdi, %[out_rdi]\n\t"
		     "mov %%rsi, %[out_rsi]\n\t"
		     "mov %%rdx, %[out_rdx]\n\t"
		     "mov %%rsp, %[out_rsp]\n\t"
		     "call *%%rax\n\t"
		     : [out_func] "=m"(elf_call_snapshot_func_addr),
		       [out_rdi] "=m"(elf_call_snapshot_rdi),
		       [out_rsi] "=m"(elf_call_snapshot_rsi),
		       [out_rdx] "=m"(elf_call_snapshot_rdx),
		       [out_rsp] "=m"(elf_call_snapshot_rsp)
		     : [fnaddr] "r"(fnaddr), [a1] "r"(header.entry),
		       [a2] "r"(user_stack_top), [a3] "r"(pd_phys)
		     : "rax", "rdi", "rsi", "rdx");

	// ここには戻ってこないはず
	printk("ELF: ERROR - returned from usermode!\n");

	// ここには戻ってこない
	printk("ELF: ERROR - returned from usermode!\n");

	return 0;
}