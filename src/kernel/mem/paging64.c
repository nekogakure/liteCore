#include <util/config.h>
#include <stdint.h>
#include <mem/paging.h>
#include <mem/map.h>
#include <mem/vmem.h>
#include <stddef.h>
#include <util/console.h>

// カーネルPML4の物理アドレスを保持
static uint64_t kernel_pml4_phys = 0;

// TLB内の単一ページを無効化するヘルパー
static inline void invlpg(void *addr) {
	asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

/**
 * @brief 64ビット4レベルページング用のマッピング関数
 * @param pml4_phys PML4テーブルの物理アドレス
 * @param phys マッピングする物理アドレス
 * @param virt マッピング先の仮想アドレス
 * @param flags ページフラグ (PAGING_PRESENT | PAGING_RW | PAGING_USER など)
 * @return 0: 成功 -1: 失敗
 */
int map_page_64(uint64_t pml4_phys, uint64_t phys, uint64_t virt,
		uint32_t flags) {
	if ((flags & PAGING_PRESENT) == 0)
		flags |= PAGING_PRESENT;

	// 64ビットページングのインデックス計算
	uint64_t pml4_idx = (virt >> 39) & 0x1FF; // bits 47-39
	uint64_t pdpt_idx = (virt >> 30) & 0x1FF; // bits 38-30
	uint64_t pd_idx = (virt >> 21) & 0x1FF; // bits 29-21
	uint64_t pt_idx = (virt >> 12) & 0x1FF; // bits 20-12

	/* Convert PML4 physical address to a kernel virtual pointer so we
	 * can access and modify the page tables regardless of current CR3
	 * mapping mode. vmem_phys_to_virt64 returns UINT64_MAX on error. */
	uint64_t pml4_virt = vmem_phys_to_virt64(pml4_phys);
	if (pml4_virt == UINT64_MAX) {
		printk("map_page_64: vmem_phys_to_virt64 returned error for pml4_phys=0x%016lx\n",
		       (unsigned long)pml4_phys);
		return -1;
	}
	uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_virt;
	uint64_t pml4_entry = pml4[pml4_idx];

	// PML4エントリをチェック/作成
	if ((pml4_entry & PAGING_PRESENT) == 0) {
		void *pdpt_virt = alloc_page_table();
		if (!pdpt_virt) {
			printk("map_page_64: Failed to allocate PDPT\n");
			return -1;
		}
		/* Zero the new table (alloc_page_table returns a virtual pointer).
		 * Then obtain the physical address for the entry via vmem_virt_to_phys64. */
		uint64_t *pdpt_clear = (uint64_t *)pdpt_virt;
		for (int i = 0; i < 512; i++) {
			pdpt_clear[i] = 0;
		}
		uint64_t pdpt_phys =
			vmem_virt_to_phys64((uint64_t)(uintptr_t)pdpt_virt);
		if (pdpt_phys == UINT64_MAX) {
			printk("map_page_64: vmem_virt_to_phys64 failed for pdpt_virt=0x%016lx\n",
			       (unsigned long)(uintptr_t)pdpt_virt);
			return -1;
		}
		uint64_t entry = (pdpt_phys & 0xFFFFFFFFFFFFF000ULL) |
				 PAGING_PRESENT | PAGING_RW | PAGING_USER;
		entry &= ~(1ULL << 63); // NXビットをクリア
		pml4[pml4_idx] = entry;
	} else {
		// 既存のエントリ - NXビットをクリア
		pml4[pml4_idx] &= ~(1ULL << 63);
	}

	// PDPTテーブルにアクセス
	uint64_t pdpt_phys = pml4[pml4_idx] & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pdpt_virt_ptr = vmem_phys_to_virt64(pdpt_phys);
	if (pdpt_virt_ptr == UINT64_MAX) {
		printk("map_page_64: vmem_phys_to_virt64 returned error for pdpt_phys=0x%016lx\n",
		       (unsigned long)pdpt_phys);
		return -1;
	}
	uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_virt_ptr;

	// PDPTエントリをチェック/作成
	if ((pdpt[pdpt_idx] & PAGING_PRESENT) == 0) {
		void *pd_virt = alloc_page_table();
		if (!pd_virt) {
			printk("map_page_64: Failed to allocate PD\n");
			return -1;
		}
		uint64_t *pd_clear = (uint64_t *)pd_virt;
		for (int i = 0; i < 512; i++) {
			pd_clear[i] = 0;
		}
		uint64_t pd_phys =
			vmem_virt_to_phys64((uint64_t)(uintptr_t)pd_virt);
		if (pd_phys == UINT64_MAX) {
			printk("map_page_64: vmem_virt_to_phys64 failed for pd_virt=0x%016lx\n",
			       (unsigned long)(uintptr_t)pd_virt);
			return -1;
		}
		uint64_t entry = (pd_phys & 0xFFFFFFFFFFFFF000ULL) |
				 PAGING_PRESENT | PAGING_RW | PAGING_USER;
		entry &= ~(1ULL << 63); // NXビットをクリア
		pdpt[pdpt_idx] = entry;
	} else {
		// 既存のエントリ - NXビットをクリア
		pdpt[pdpt_idx] &= ~(1ULL << 63);
	}

	// PDテーブルにアクセス
	uint64_t pd_phys_addr = pdpt[pdpt_idx] & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pd_virt_ptr = vmem_phys_to_virt64(pd_phys_addr);
	if (pd_virt_ptr == UINT64_MAX) {
		printk("map_page_64: vmem_phys_to_virt64 returned error for pd_phys=0x%016lx\n",
		       (unsigned long)pd_phys_addr);
		return -1;
	}
	uint64_t *pd = (uint64_t *)(uintptr_t)pd_virt_ptr;

	// PDエントリをチェック/作成
	// 既存エントリが2MBラージページ（PS bit=1）の場合は分割が必要
	if ((pd[pd_idx] & PAGING_PRESENT) == 0) {
		// エントリが存在しない場合：新しいPTを作成
		void *pt_virt = alloc_page_table();
		if (!pt_virt) {
			printk("map_page_64: Failed to allocate PT\n");
			return -1;
		}
		uint64_t *pt_clear = (uint64_t *)pt_virt;
		for (int i = 0; i < 512; i++) {
			pt_clear[i] = 0;
		}
		uint64_t pt_phys =
			vmem_virt_to_phys64((uint64_t)(uintptr_t)pt_virt);
		if (pt_phys == UINT64_MAX) {
			printk("map_page_64: vmem_virt_to_phys64 failed for pt_virt=0x%016lx\n",
			       (unsigned long)(uintptr_t)pt_virt);
			return -1;
		}
		uint64_t entry = (pt_phys & 0xFFFFFFFFFFFFF000ULL) |
				 PAGING_PRESENT | PAGING_RW | PAGING_USER;
		entry &= ~(1ULL << 63); // NXビットをクリア
		pd[pd_idx] = entry;
	} else if (pd[pd_idx] & (1ULL << 7)) {
		uint64_t large_page_base = pd[pd_idx] & 0xFFFFFFFFFFE00000ULL;
		uint64_t large_page_flags = pd[pd_idx] & 0xFFF;

		// 新しいPTを割り当て
		void *pt_virt = alloc_page_table();
		if (!pt_virt) {
			printk("map_page_64: Failed to allocate PT for page split\n");
			return -1;
		}
		uint64_t *pt_split = (uint64_t *)pt_virt;

		// 2MBラージページを512個の4KBページに分割
		for (int i = 0; i < 512; i++) {
			uint64_t page_phys =
				large_page_base + ((uint64_t)i * 0x1000);
			// PS bitを除去し、他のフラグは保持
			pt_split[i] = (page_phys & 0xFFFFFFFFFFFFF000ULL) |
				      (large_page_flags & ~(1ULL << 7));
		}

		uint64_t pt_phys =
			vmem_virt_to_phys64((uint64_t)(uintptr_t)pt_virt);
		if (pt_phys == UINT64_MAX) {
			printk("map_page_64: vmem_virt_to_phys64 failed for pt_virt=0x%016lx\n",
			       (unsigned long)(uintptr_t)pt_virt);
			return -1;
		}

		// PDエントリを新しいPTを指すように更新（PS bitをクリア）
		pd[pd_idx] = (pt_phys & 0xFFFFFFFFFFFFF000ULL) |
			     PAGING_PRESENT | PAGING_RW | PAGING_USER;
		pd[pd_idx] &= ~(1ULL << 63); // NXビットをクリア

		// TLB を無効化（2MB分）
		for (uint64_t i = 0; i < 512; i++) {
			uint64_t flush_addr =
				(virt & 0xFFFFFFFFFFE00000ULL) + (i * 0x1000);
			invlpg((void *)(uintptr_t)flush_addr);
		}
	} else {
		// 既存のエントリ（4KB PT）- NXビットをクリア
		pd[pd_idx] &= ~(1ULL << 63);
	}

	// PTテーブルにアクセス
	uint64_t pt_phys_addr = pd[pd_idx] & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pt_virt_ptr = vmem_phys_to_virt64(pt_phys_addr);
	if (pt_virt_ptr == UINT64_MAX) {
		printk("map_page_64: vmem_phys_to_virt64 returned error for pt_phys=0x%016lx\n",
		       (unsigned long)pt_phys_addr);
		return -1;
	}
	uint64_t *pt = (uint64_t *)(uintptr_t)pt_virt_ptr;

	// 最終的なマッピングを設定
	// NXビット（bit 63）を明示的にクリアして実行可能にする
	uint64_t entry = (phys & 0xFFFFFFFFFFFFF000ULL) | (flags & 0xFFF);
	entry &= ~(1ULL << 63); // NXビットをクリア（実行可能）
	pt[pt_idx] = entry;

	// TLBを無効化
	invlpg((void *)(uintptr_t)virt);

	return 0;
}

/**
 * @brief 現在のCR3（PML4）を使用してページをマッピング
 */
int map_page_current_64(uint64_t phys, uint64_t virt, uint32_t flags) {
	uint64_t cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	return map_page_64(cr3, phys, virt, flags);
}

/**
 * @brief UEFIのPML4を新しい書き込み可能なPML4にコピーしてCR3を切り替える
 * カーネル初期化時に1度だけ呼ばれる
 */
void paging64_init_kernel_pml4(void) {
	// 現在のCR3（UEFIのPML4）を取得（物理アドレス）
	uint64_t uefi_cr3;
	asm volatile("mov %%cr3, %0" : "=r"(uefi_cr3));
	uint64_t uefi_pml4_virt = vmem_phys_to_virt64(uefi_cr3);
	if (uefi_pml4_virt == UINT64_MAX) {
		printk("paging64_init_kernel_pml4: vmem_phys_to_virt64 failed for uefi_cr3=0x%016lx\n",
		       (unsigned long)uefi_cr3);
		return;
	}
	uint64_t *uefi_pml4 = (uint64_t *)(uintptr_t)uefi_pml4_virt;

	// 新しいPML4用のフレームを割り当て
	void *new_pml4_frame = alloc_frame();
	if (!new_pml4_frame) {
		printk("paging64_init_kernel_pml4: Failed to allocate new PML4\n");
		return;
	}

	uint64_t new_pml4_phys = (uint64_t)(uintptr_t)new_pml4_frame;
	uint64_t new_pml4_virt = vmem_phys_to_virt64(new_pml4_phys);
	if (new_pml4_virt == UINT64_MAX) {
		printk("paging64_init_kernel_pml4: vmem_phys_to_virt64 failed for new_pml4_phys=0x%016lx\n",
		       (unsigned long)new_pml4_phys);
		return;
	}
	uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_virt;

	// UEFIのPML4エントリをすべてコピー
	for (int i = 0; i < 512; i++) {
		new_pml4[i] = uefi_pml4[i];
	}

	// 低位メモリ（0x0～0xFFFFFFFF, 4GB）をアイデンティティマッピングで追加
	// 新しいPDPTを作成（PML4[0]用）
	void *pdpt_frame = alloc_frame();
	if (!pdpt_frame) {
		printk("paging64_init_kernel_pml4: Failed to allocate PDPT\n");
		return;
	}
	uint64_t pdpt_phys = (uint64_t)(uintptr_t)pdpt_frame;

	uint64_t pdpt_virt = vmem_phys_to_virt64(pdpt_phys);
	if (pdpt_virt == UINT64_MAX) {
		printk("paging64_init_kernel_pml4: vmem_phys_to_virt64 failed for pdpt_phys=0x%016lx\n",
		       (unsigned long)pdpt_phys);
		return;
	}
	// PDPTをゼロクリア
	uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_virt;
	for (int i = 0; i < 512; i++) {
		pdpt[i] = 0;
	}

	// PDPT[0..3]用のPDを作成（0x0～0xFFFFFFFF, 4GB = 4 x 1GB）
	for (int pdpt_idx = 0; pdpt_idx < 4; pdpt_idx++) {
		void *pd_frame = alloc_frame();
		if (!pd_frame) {
			printk("paging64_init_kernel_pml4: Failed to allocate PD[%d]\n",
			       pdpt_idx);
			return;
		}
		uint64_t pd_phys = (uint64_t)(uintptr_t)pd_frame;
		uint64_t pd_virt = vmem_phys_to_virt64(pd_phys);
		if (pd_virt == UINT64_MAX) {
			printk("paging64_init_kernel_pml4: vmem_phys_to_virt64 failed for pd_phys=0x%016lx\n",
			       (unsigned long)pd_phys);
			return;
		}
		uint64_t *pd = (uint64_t *)(uintptr_t)pd_virt;

		// PDに512個の2MBページエントリを設定
		for (int i = 0; i < 512; i++) {
			uint64_t phys_addr =
				((uint64_t)pdpt_idx * 0x40000000ULL) +
				((uint64_t)i * 0x200000ULL);
			pd[i] = phys_addr |
				0x87; // Present, RW, User, PS (2MB page)
		}

		// PDPTにPDを設定
		pdpt[pdpt_idx] = (pd_phys & 0xFFFFFFFFFFFFF000ULL) |
				 0x7; // Present, RW, User
	}

	// PML4[0]にPDPTを設定
	new_pml4[0] = (pdpt_phys & 0xFFFFFFFFFFFFF000ULL) |
		      0x7; // Present, RW, User

	// 新しいPML4に切り替え
	asm volatile("mov %0, %%cr3" ::"r"(new_pml4_phys) : "memory");

	// カーネルPML4の物理アドレスを保存
	kernel_pml4_phys = new_pml4_phys;
}

/**
 * @brief カーネルPML4の物理アドレスを取得
 */
uint64_t paging64_get_kernel_pml4(void) {
	return kernel_pml4_phys;
}

/**
 * @brief 新しいユーザープロセス用のPML4を作成
 * カーネル空間のマッピング(PML4の上位256エントリ)はカーネルPML4からコピー
 * ユーザー空間(下位256エントリ)は空で初期化
 * @return 新しいPML4の物理アドレス、失敗時は0
 */
uint64_t paging64_create_user_pml4(void) {
	if (kernel_pml4_phys == 0) {
		printk("paging64_create_user_pml4: Kernel PML4 not initialized\n");
		return 0;
	}

	// 新しいPML4を割り当て
	void *new_pml4_frame = alloc_frame();
	if (!new_pml4_frame) {
		printk("paging64_create_user_pml4: Failed to allocate PML4\n");
		return 0;
	}

	uint64_t new_pml4_phys = (uint64_t)(uintptr_t)new_pml4_frame;
	/* Convert physical addresses to kernel virtual addresses so we can
	 * safely read/write the tables regardless of current CR3 mapping. */
	uint64_t new_pml4_virt = vmem_phys_to_virt64(new_pml4_phys);
	if (new_pml4_virt == UINT64_MAX) {
		printk("paging64_create_user_pml4: vmem_phys_to_virt64 failed for new_pml4_phys=0x%016lx\n",
		       (unsigned long)new_pml4_phys);
		return 0;
	}
	uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_virt;

	uint64_t kernel_pml4_virt = vmem_phys_to_virt64(kernel_pml4_phys);
	if (kernel_pml4_virt == UINT64_MAX) {
		printk("paging64_create_user_pml4: vmem_phys_to_virt64 failed for kernel_pml4_phys=0x%016lx\n",
		       (unsigned long)kernel_pml4_phys);
		return 0;
	}
	uint64_t *kernel_pml4 = (uint64_t *)(uintptr_t)kernel_pml4_virt;

	// ユーザー空間(PML4[1-255])をゼロクリア
	// PML4[0]は後でカーネルからコピーする（カーネルコードへのアクセスに必要）
	for (int i = 1; i < 256; i++) {
		new_pml4[i] = 0;
	}

	// カーネル空間(PML4[256-511])をカーネルPML4からコピー
	for (int i = 256; i < 512; i++) {
		new_pml4[i] = kernel_pml4[i];
	}

	/*
     * IMPORTANT: Copy kernel_pml4[0] to enable access to kernel code/data
     * in low memory (identity-mapped region) even after CR3 switch.
     * Without this, iretq instruction itself (which is in kernel code at low
     * addresses) would cause a page fault when CR3 is switched to user PML4.
     * 
     * User mappings at low addresses will still work because map_page_64
     * creates 4KB page tables which override the 2MB large pages from kernel.
     */
	new_pml4[0] = kernel_pml4[0];

	return new_pml4_phys;
}
