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

	// PML4テーブルにアクセス - アイデンティティマッピングを仮定
	uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
	uint64_t pml4_entry = pml4[pml4_idx];

	// PML4エントリをチェック/作成
	if ((pml4_entry & PAGING_PRESENT) == 0) {
		void *pdpt_virt = alloc_page_table();
		if (!pdpt_virt) {
			printk("map_page_64: Failed to allocate PDPT\n");
			return -1;
		}
		// 64ビット版として確実にゼロクリア（alloc_page_tableは32ビット版なので）
		uint64_t *pdpt_clear = (uint64_t *)pdpt_virt;
		for (int i = 0; i < 512; i++) {
			pdpt_clear[i] = 0;
		}
		// 仮想→物理変換（アイデンティティマッピングなら同じ値）
		uint64_t pdpt_phys = (uint64_t)(uintptr_t)pdpt_virt;
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
	uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

	// PDPTエントリをチェック/作成
	if ((pdpt[pdpt_idx] & PAGING_PRESENT) == 0) {
		void *pd_virt = alloc_page_table();
		if (!pd_virt) {
			printk("map_page_64: Failed to allocate PD\n");
			return -1;
		}
		// 64ビット版として確実にゼロクリア
		uint64_t *pd_clear = (uint64_t *)pd_virt;
		for (int i = 0; i < 512; i++) {
			pd_clear[i] = 0;
		}
		uint64_t pd_phys = (uint64_t)(uintptr_t)pd_virt;
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
	uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys_addr;

	// PDエントリをチェック/作成
	if ((pd[pd_idx] & PAGING_PRESENT) == 0) {
		void *pt_virt = alloc_page_table();
		if (!pt_virt) {
			printk("map_page_64: Failed to allocate PT\n");
			return -1;
		}
		// 64ビット版として確実にゼロクリア
		uint64_t *pt_clear = (uint64_t *)pt_virt;
		for (int i = 0; i < 512; i++) {
			pt_clear[i] = 0;
		}
		uint64_t pt_phys = (uint64_t)(uintptr_t)pt_virt;
		uint64_t entry = (pt_phys & 0xFFFFFFFFFFFFF000ULL) |
				 PAGING_PRESENT | PAGING_RW | PAGING_USER;
		entry &= ~(1ULL << 63); // NXビットをクリア
		pd[pd_idx] = entry;
	} else {
		// 既存のエントリ - NXビットをクリア
		pd[pd_idx] &= ~(1ULL << 63);
	}

	// PTテーブルにアクセス
	uint64_t pt_phys_addr = pd[pd_idx] & 0xFFFFFFFFFFFFF000ULL;
	uint64_t *pt = (uint64_t *)(uintptr_t)pt_phys_addr;

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
	// 現在のCR3（UEFIのPML4）を取得
	uint64_t uefi_cr3;
	asm volatile("mov %%cr3, %0" : "=r"(uefi_cr3));
	uint64_t *uefi_pml4 = (uint64_t *)(uintptr_t)uefi_cr3;

	// 新しいPML4用のフレームを割り当て
	void *new_pml4_frame = alloc_frame();
	if (!new_pml4_frame) {
		printk("paging64_init_kernel_pml4: Failed to allocate new PML4\n");
		return;
	}

	uint64_t new_pml4_phys = (uint64_t)(uintptr_t)new_pml4_frame;
	uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_phys;

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

	// PDPTをゼロクリア
	uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;
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
		uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;

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

	printk("paging64_init_kernel_pml4: Switched from UEFI PML4 (0x%lx) to kernel PML4 (0x%lx)\n",
	       uefi_cr3, new_pml4_phys);
	printk("paging64_init_kernel_pml4: Added identity mapping for low 4GB (0x0-0xFFFFFFFF)\n");
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
	uint64_t *new_pml4 = (uint64_t *)(uintptr_t)new_pml4_phys;
	uint64_t *kernel_pml4 = (uint64_t *)(uintptr_t)kernel_pml4_phys;

	// ユーザー空間(PML4[0-255])をゼロクリア
	for (int i = 0; i < 256; i++) {
		new_pml4[i] = 0;
	}

	// カーネル空間(PML4[256-511])をカーネルPML4からコピー
	for (int i = 256; i < 512; i++) {
		new_pml4[i] = kernel_pml4[i];
	}

	// 低位4GBのidentity mappingをコピー(PML4[0])
	// これはalloc_frame()などカーネル機能が使用する
	new_pml4[0] = kernel_pml4[0];

	return new_pml4_phys;
}
