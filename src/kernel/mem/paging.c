#include <util/config.h>
#include <stdint.h>
#include <mem/paging.h>
#include <mem/map.h>
#include <mem/vmem.h>
#include <stddef.h>
#include <util/console.h>
#include <interrupt/irq.h>

// ページディレクトリ（4KBアライン）
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
// 最初の4MBの同一マッピング用テーブル（4KBアライン）
static uint32_t first_table[1024] __attribute__((aligned(4096)));

// TLB内の単一ページを無効化するヘルパー
static inline void invlpg(void *addr) {
	asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
}

// ゼロクリアされたページテーブルを確保する（仮想アドレスを返す。失敗時NULL）
void *alloc_page_table(void) {
	void *frame = alloc_frame();
	if (!frame)
		return NULL;
	// alloc_frame() は物理フレームの物理アドレスを返す実装の可能性がある。
	// 実行環境によらず仮想ポインタを得るため vmem_phys_to_virt を用いる。
	uint32_t phys = (uint32_t)(uintptr_t)frame;
	uint32_t virt = vmem_phys_to_virt(phys);
	if (virt == 0) {
		printk("alloc_page_table: vmem_phys_to_virt returned 0 for phys=0x%x\n",
		       (unsigned)phys);
		return NULL;
	}
	printk("alloc_page_table: clearing table at virt=0x%x (phys=0x%x)\n",
	       (unsigned)virt, (unsigned)phys);
	// ページングが有効化されていない場合、virtはphysと同じ（アイデンティティ）
	// しかし、physが1MBを超える場合、アクセスできない可能性がある
	// 安全のため、物理アドレスで直接アクセス
	uint32_t *tbl_phys = (uint32_t *)(uintptr_t)phys;
	for (size_t i = 0; i < 1024; ++i)
		tbl_phys[i] = 0;
	printk("alloc_page_table: table cleared\n");
	// return the virtual pointer for convenience to callers
	return (void *)(uintptr_t)virt;
}

/**
 * @fn map_page
 * @brief 物理アドレスphysを仮想アドレスvirtにflags属性でマッピングする
 * @return 0: 成功 -1: 失敗
 */
int map_page(uint32_t phys, uint32_t virt, uint32_t flags) {
	if ((flags & PAGING_PRESENT) == 0)
		flags |= PAGING_PRESENT;
	uint32_t pd_idx = (virt >> 22) & 0x3FF;
	uint32_t pt_idx = (virt >> 12) & 0x3FF;

	uint32_t pde = page_directory[pd_idx];
	uint32_t *pt;
	if ((pde & PAGING_PRESENT) == 0) {
		void *new_pt_virt = alloc_page_table();
		if (!new_pt_virt) {
			printk("map_page: alloc_page_table failed for pd_idx=%u\n",
			       (unsigned)pd_idx);
			return -1;
		}
		printk("map_page: new_pt_virt=0x%x, converting to phys\n",
		       (unsigned)(uintptr_t)new_pt_virt);
		uint32_t new_pt_phys =
			vmem_virt_to_phys((uint32_t)(uintptr_t)new_pt_virt);
		printk("map_page: new_pt_phys=0x%x\n", (unsigned)new_pt_phys);
		if (new_pt_phys == 0) {
			printk("map_page: vmem_virt_to_phys returned 0 for new_pt_virt=0x%x\n",
			       (unsigned)(uintptr_t)new_pt_virt);
			return -1;
		}
		page_directory[pd_idx] = (new_pt_phys & 0xFFFFF000) |
					 (PAGING_PRESENT | PAGING_RW);
		pt = (uint32_t *)new_pt_virt;
	} else {
		uint32_t pt_phys = pde & 0xFFFFF000;
		uint32_t pt_virt = vmem_phys_to_virt(pt_phys);
		if (pt_virt == 0) {
			printk("map_page: vmem_phys_to_virt returned 0 for pt_phys=0x%x (pd_idx=%u)\n",
			       (unsigned)pt_phys, (unsigned)pd_idx);
			return -1;
		}
		pt = (uint32_t *)(uintptr_t)pt_virt;
	}

	pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF);
	invlpg((void *)(uintptr_t)virt);
	return 0;
}

/**
 * @fn map_page_pd
 * @brief 指定のページディレクトリ（物理アドレスpd_phys）へ物理アドレスphysを
 *        仮想アドレスvirtにflags属性でマッピングする
 * @return 0: 成功 -1: 失敗
 */
int map_page_pd(uint32_t pd_phys, uint32_t phys, uint32_t virt,
		uint32_t flags) {
	if ((flags & PAGING_PRESENT) == 0)
		flags |= PAGING_PRESENT;

	uint32_t pd_idx = (virt >> 22) & 0x3FF;
	uint32_t pt_idx = (virt >> 12) & 0x3FF;

	uint32_t pd_virt = vmem_phys_to_virt(pd_phys);
	if (pd_virt == 0) {
		printk("map_page_pd: vmem_phys_to_virt returned 0 for pd_phys=0x%x\n",
		       (unsigned)pd_phys);
		return -1;
	}

	uint32_t *pd = (uint32_t *)(uintptr_t)pd_virt;
	uint32_t pde = pd[pd_idx];
	uint32_t *pt;
	if ((pde & PAGING_PRESENT) == 0) {
		void *new_pt_virt = alloc_page_table();
		if (!new_pt_virt) {
			printk("map_page_pd: alloc_page_table failed for pd_idx=%u\n",
			       (unsigned)pd_idx);
			return -1;
		}
		uint32_t new_pt_phys =
			vmem_virt_to_phys((uint32_t)(uintptr_t)new_pt_virt);
		if (new_pt_phys == 0) {
			printk("map_page_pd: vmem_virt_to_phys returned 0 for new_pt_virt=0x%x\n",
			       (unsigned)(uintptr_t)new_pt_virt);
			return -1;
		}
		pd[pd_idx] = (new_pt_phys & 0xFFFFF000) |
			     (PAGING_PRESENT | PAGING_RW);
		pt = (uint32_t *)new_pt_virt;
	} else {
		uint32_t pt_phys = pde & 0xFFFFF000;
		uint32_t pt_virt = vmem_phys_to_virt(pt_phys);
		if (pt_virt == 0) {
			printk("map_page_pd: vmem_phys_to_virt returned 0 for pt_phys=0x%x (pd_idx=%u)\n",
			       (unsigned)pt_phys, (unsigned)pd_idx);
			return -1;
		}
		pt = (uint32_t *)(uintptr_t)pt_virt;
	}

	pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF);
	invlpg((void *)(uintptr_t)virt);
	return 0;
}

/**
 * @fn unmap_page_pd
 * @brief 指定ページディレクトリ(pd_phys)からvirtをアンマップする
 */
int unmap_page_pd(uint32_t pd_phys, uint32_t virt) {
	uint32_t pd_idx = (virt >> 22) & 0x3FF;
	uint32_t pt_idx = (virt >> 12) & 0x3FF;

	uint32_t pd_virt = vmem_phys_to_virt(pd_phys);
	if (pd_virt == 0) {
		printk("unmap_page_pd: vmem_phys_to_virt returned 0 for pd_phys=0x%x\n",
		       (unsigned)pd_phys);
		return -1;
	}
	uint32_t *pd = (uint32_t *)(uintptr_t)pd_virt;

	uint32_t pde = pd[pd_idx];
	if ((pde & PAGING_PRESENT) == 0)
		return -1;
	uint32_t pt_phys = pde & 0xFFFFF000;
	uint32_t pt_virt = vmem_phys_to_virt(pt_phys);
	if (pt_virt == 0) {
		printk("unmap_page_pd: vmem_phys_to_virt returned 0 for pt_phys=0x%x (pd_idx=%u)\n",
		       (unsigned)pt_phys, (unsigned)pd_idx);
		return -1;
	}
	uint32_t *pt = (uint32_t *)(uintptr_t)pt_virt;
	if ((pt[pt_idx] & PAGING_PRESENT) == 0)
		return -1;
	pt[pt_idx] = 0;
	invlpg((void *)(uintptr_t)virt);

	// check if page table became empty -> free it and clear PDE
	int empty = 1;
	for (int i = 0; i < 1024; ++i) {
		if (pt[i] & PAGING_PRESENT) {
			empty = 0;
			break;
		}
	}
	if (empty) {
		pd[pd_idx] = 0x00000000;
		free_frame((void *)(uintptr_t)pt_phys);
	}

	return 0;
}

/**
 * @fn unmap_page
 * @brief 仮想アドレスvirtに対応するページをアンマップする
 * @param virt アンマップする仮想アドレス
 * @return 0: 成功 -1: 失敗
 */
int unmap_page(uint32_t virt) {
	uint32_t pd_idx = (virt >> 22) & 0x3FF;
	uint32_t pt_idx = (virt >> 12) & 0x3FF;

	uint32_t pde = page_directory[pd_idx];
	if ((pde & PAGING_PRESENT) == 0)
		return -1;
	uint32_t pt_phys = pde & 0xFFFFF000;
	uint32_t pt_virt = vmem_phys_to_virt(pt_phys);
	if (pt_virt == 0) {
		printk("unmap_page: vmem_phys_to_virt returned 0 for pt_phys=0x%x (pd_idx=%u)\n",
		       (unsigned)pt_phys, (unsigned)pd_idx);
		return -1;
	}
	uint32_t *pt = (uint32_t *)(uintptr_t)pt_virt;
	if ((pt[pt_idx] & PAGING_PRESENT) == 0)
		return -1;
	pt[pt_idx] = 0;
	invlpg((void *)(uintptr_t)virt);

	/* check if the page table became empty -> free it and clear PDE */
	int empty = 1;
	for (int i = 0; i < 1024; ++i) {
		if (pt[i] & PAGING_PRESENT) {
			empty = 0;
			break;
		}
	}
	if (empty) {
		uint32_t pt_phys = vmem_virt_to_phys((uint32_t)(uintptr_t)pt);
		if (pt_phys == 0) {
			printk("unmap_page: vmem_virt_to_phys returned 0 for pt_virt=0x%x\n",
			       (unsigned)(uintptr_t)pt);
		} else {
			page_directory[pd_idx] = 0x00000000;
			free_frame((void *)(uintptr_t)pt_phys);
		}
	}

	return 0;
}

/**
 * @fn paging_init_identity
 * @brief 最初のmap_mb MB分を同一マッピングで初期化する
 * @param map_mb 同一マッピングするMB数
 */
void paging_init_identity(uint32_t map_mb) {
	uint32_t pages = ((map_mb * 1024u * 1024u) + 0xFFF) / 0x1000;

	// 最初のテーブル: 最初の4MBを同一マッピング（1024エントリ）
	for (uint32_t i = 0; i < 1024; ++i) {
		first_table[i] = (i * 0x1000) | 3u; // present + rw
	}

	// ディレクトリエントリ0はfirst_tableを指す。first_tableは静的で既に仮想アドレスとしてアクセス可能だが、PDEには物理アドレスを置く必要がある。
	uint32_t first_table_phys =
		vmem_virt_to_phys((uint32_t)(uintptr_t)first_table);
	if (first_table_phys == 0) {
		printk("paging_init_identity: vmem_virt_to_phys returned 0 for first_table!\n");
		return;
	}
	page_directory[0] = (first_table_phys & 0xFFFFF000) | 3u;

	// 残りのPDEをnot presentにする
	for (uint32_t i = 1; i < 1024; ++i)
		page_directory[i] = 0x00000000;

	printk("paging: identity map initialized for %u MB (pages=%u)\n",
	       (unsigned)map_mb, (unsigned)pages);
}

/**
 * @fn map_range
 * @brief 指定範囲を連続ページとしてマップする
 */
int map_range(uint32_t phys_start, uint32_t virt_start, size_t size,
	      uint32_t flags) {
	if (phys_start % 0x1000 || virt_start % 0x1000)
		return -1;
	uint32_t pages = (size + 0xFFF) / 0x1000;
	for (uint32_t i = 0; i < pages; ++i) {
		if (map_page(phys_start + i * 0x1000, virt_start + i * 0x1000,
			     flags) != 0) {
			return -1;
		}
	}
	return 0;
}

void page_fault_handler_ex(uint32_t vec, uint32_t error_code, uint32_t eip) {
	uint64_t fault_addr;
	asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
	printk("PAGE FAULT: vec=%u err=0x%x eip=0x%x cr2=0x%lx\n",
	       (unsigned)vec, (unsigned)error_code, (unsigned)eip,
	       (unsigned long)fault_addr);
	while (1) {
	}
}

/**
 * @fn paging_enable
 * @brief ページングを有効化
 */
void paging_enable(void) {
	// 現在のページング状態を確認
	uint64_t cr0, cr3, cr4;
	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	printk("paging_enable: Current CR0=0x%lx CR3=0x%lx CR4=0x%lx\n",
	       (unsigned long)cr0, (unsigned long)cr3, (unsigned long)cr4);
	printk("paging_enable: PG bit=%d, PAE bit=%d\n", (int)((cr0 >> 31) & 1),
	       (int)((cr4 >> 5) & 1));

	// 注意: x86-64ロングモードでは、UEFIがすでに64ビットページングを設定しています。
	// 現在の32ビットページング構造（2レベル: PD→PT）は互換性がありません。
	// 64ビットモードでは4レベル（PML4→PDPT→PD→PT）構造が必要です。
	// そのため、UEFIが設定したページテーブルをそのまま使用します。
	printk("paging_enable: Skipping custom paging setup (using UEFI page tables)\n");
	printk("paging_enable: WARNING - 32-bit paging structures are incompatible with x86-64 long mode\n");
}

void page_fault_handler(uint32_t vec) {
	(void)vec;
	uint64_t fault_addr;
	asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
	printk("PAGE FAULT at 0x%lx\n", (unsigned long)fault_addr);
	while (1) {
	}
}