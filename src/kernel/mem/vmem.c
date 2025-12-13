#include <util/console.h>
#include <stdint.h>
#include <mem/vmem.h>
#include <interrupt/irq.h>

// 仮想メモリオフセット
static int32_t vmem_offset = 0;

// デフォルトの物理アドレス→仮想アドレス変換関数
static uint32_t default_phys2virt(uint32_t phys) {
	if (vmem_offset == 0)
		return phys;
	return (uint32_t)((int32_t)phys + vmem_offset);
}

// 現在の仮想メモリモード
static vmem_mode_t current_mode = VMEM_MODE_IDENTITY;
// 物理→仮想変換関数
static vmem_phys2virt_fn phys2virt = NULL;

// 仮想アドレス→物理アドレス変換
uint32_t vmem_virt_to_phys(uint32_t virt) {
	if (current_mode == VMEM_MODE_IDENTITY) {
		return virt;
	}
	if (current_mode == VMEM_MODE_OFFSET) {
		if ((int32_t)virt - vmem_offset < 0)
			return 0;
		uint32_t phys = (uint32_t)((int32_t)virt - vmem_offset);
		printk("vmem_virt_to_phys: offset -> phys=0x%x\n",
		       (unsigned)phys);
		return phys;
	}

	uint64_t cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	uint32_t pd_phys = cr3 & 0xFFFFF000;
	uint32_t pd_virt =
		(phys2virt ? phys2virt(pd_phys) : default_phys2virt(pd_phys));
	if (pd_virt == 0) {
		printk("vmem_virt_to_phys: phys2virt returned 0 for pd_phys=0x%x\n",
		       (unsigned)pd_phys);
		return 0;
	}

	uint32_t pd_idx = (virt >> 22) & 0x3FF;
	uint32_t pt_idx = (virt >> 12) & 0x3FF;

	uint32_t *pd = (uint32_t *)(uintptr_t)pd_virt;
	uint32_t pde = pd[pd_idx];
	if ((pde & 0x1) == 0) {
		printk("vmem_virt_to_phys: PDE not present pd_idx=%u pde=0x%x\n",
		       (unsigned)pd_idx, (unsigned)pde);
		return 0; // 存在しない
	}

	// 4MBページのチェック
	if (pde & 0x80) {
		// 4MBページ: ビット22..31が物理ベース
		uint32_t page_base = pde & 0xFFC00000;
		uint32_t offset = virt & 0x003FFFFF;
		uint32_t phys = page_base | offset;
		printk("vmem_virt_to_phys: 4MB page -> phys=0x%x\n",
		       (unsigned)phys);
		return phys;
	}

	uint32_t pt_phys = pde & 0xFFFFF000;
	uint32_t pt_virt =
		(phys2virt ? phys2virt(pt_phys) : default_phys2virt(pt_phys));
	if (pt_virt == 0) {
		printk("vmem_virt_to_phys: phys2virt returned 0 for pt_phys=0x%x\n",
		       (unsigned)pt_phys);
		return 0;
	}
	uint32_t *pt = (uint32_t *)(uintptr_t)pt_virt;
	uint32_t pte = pt[pt_idx];
	if ((pte & 0x1) == 0) {
		printk("vmem_virt_to_phys: PTE not present pt_idx=%u pte=0x%x\n",
		       (unsigned)pt_idx, (unsigned)pte);
		return 0;
	}
	uint32_t page_base = pte & 0xFFFFF000;
	uint32_t offset = virt & 0xFFF;
	uint32_t phys = page_base | offset;
	printk("vmem_virt_to_phys: resolved phys=0x%x\n", (unsigned)phys);
	return phys;
}

uint64_t vmem_virt_to_phys64(uint64_t virt) {
	if (current_mode == VMEM_MODE_IDENTITY) {
		return virt;
	}
	if (current_mode == VMEM_MODE_OFFSET) {
		if ((int64_t)virt - vmem_offset < 0)
			return 0;
		uint64_t phys = (uint64_t)((int64_t)virt - vmem_offset);
		return phys;
	}

	uint64_t cr3;
	asm volatile("mov %%cr3, %0" : "=r"(cr3));
	uint64_t pml4_base = cr3 & 0xFFFFFFFFFFFFF000ULL;

	/* indices */
	uint64_t pml4_idx = (virt >> 39) & 0x1FFULL;
	uint64_t pdpt_idx = (virt >> 30) & 0x1FFULL;
	uint64_t pd_idx = (virt >> 21) & 0x1FFULL;
	uint64_t pt_idx = (virt >> 12) & 0x1FFULL;
	uint64_t page_off = virt & 0xFFFULL;

	uint64_t read_entry;
	uint64_t entry;

	/* read PML4E */
	uint64_t pml4e_phys = pml4_base + (pml4_idx * 8);
	uint64_t pml4e_virt = vmem_phys_to_virt64(pml4e_phys);
	if (pml4e_virt == UINT64_MAX)
		return 0;
	read_entry = *((volatile uint64_t *)(uintptr_t)pml4e_virt);
	entry = read_entry;
	if ((entry & 0x1) == 0)
		return 0; /* not present */

	/* PDPT */
	uint64_t pdpt_base = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pdpte_phys = pdpt_base + (pdpt_idx * 8);
	uint64_t pdpte_virt = vmem_phys_to_virt64(pdpte_phys);
	if (pdpte_virt == UINT64_MAX)
		return 0;
	read_entry = *((volatile uint64_t *)(uintptr_t)pdpte_virt);
	entry = read_entry;
	if ((entry & 0x1) == 0)
		return 0;
	/* 1 GiB page? (PS bit) */
	if (entry & (1ULL << 7)) {
		uint64_t base = entry & 0xFFFFFC0000000ULL; /* bits 51:30 */
		uint64_t offset = virt & 0x3FFFFFFFULL; /* lower 30 bits */
		return base + offset;
	}

	/* PD */
	uint64_t pd_base = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pde_phys = pd_base + (pd_idx * 8);
	uint64_t pde_virt = vmem_phys_to_virt64(pde_phys);
	if (pde_virt == UINT64_MAX)
		return 0;
	read_entry = *((volatile uint64_t *)(uintptr_t)pde_virt);
	entry = read_entry;
	if ((entry & 0x1) == 0)
		return 0;
	/* 2 MiB page? (PS bit) */
	if (entry & (1ULL << 7)) {
		uint64_t base = entry & 0xFFFFFFFFFFE00000ULL; /* bits 51:21 */
		uint64_t offset = virt & 0x1FFFFFULL; /* lower 21 bits */
		return base + offset;
	}

	/* PT */
	uint64_t pt_base = entry & 0xFFFFFFFFFFFFF000ULL;
	uint64_t pte_phys = pt_base + (pt_idx * 8);
	uint64_t pte_virt = vmem_phys_to_virt64(pte_phys);
	if (pte_virt == UINT64_MAX)
		return 0;
	read_entry = *((volatile uint64_t *)(uintptr_t)pte_virt);
	entry = read_entry;
	if ((entry & 0x1) == 0)
		return 0;

	uint64_t page_base = entry & 0xFFFFFFFFFFFFF000ULL;
	return page_base + page_off;
}

// 物理アドレス→仮想アドレス変換
uint32_t vmem_phys_to_virt(uint32_t phys) {
	/* Treat UINT32_MAX as error sentinel instead of 0 to allow phys==0 */
	if (phys == UINT32_MAX) {
		return UINT32_MAX;
	}

	switch (current_mode) {
	case VMEM_MODE_IDENTITY:
		return phys;
	case VMEM_MODE_OFFSET:
		return phys + (uint32_t)vmem_offset;
	case VMEM_MODE_WALK:
		/* Prefer phys2virt if provided; otherwise fall back to offset/identity */
		if (phys2virt) {
			uint32_t v = phys2virt(phys);
			if (v != 0 && v != UINT32_MAX) {
				return v;
			}
			/* fall through to fallback below */
		}
		/* If phys2virt missing or returned invalid, try offset/identity fallback */
		if (vmem_offset != 0) {
			return phys + (uint32_t)vmem_offset;
		}
		return phys; /* identity fallback */
	default:
		return UINT32_MAX;
	}
}

uint64_t vmem_phys_to_virt64(uint64_t phys) {
	/*
	 * vmem_phys_to_virt works with 32-bit physical addresses and returns
	 * UINT32_MAX on error. The 64-bit wrapper must translate that error
	 * into UINT64_MAX so callers (the page-table walker) can detect
	 * failures reliably. Also fail if the physical address doesn't fit
	 * in 32-bits because this simple mapper cannot handle >4GiB frames
	 * without a proper phys->virt64 implementation.
	 */
	if (phys == UINT64_MAX) {
		return UINT64_MAX;
	}
	if (phys > UINT32_MAX) {
		/* unsupported: physical address out of 32-bit range */
		return UINT64_MAX;
	}
	uint32_t p32 = (uint32_t)phys;
	uint32_t v32 = vmem_phys_to_virt(p32);
	if (v32 == UINT32_MAX) {
		return UINT64_MAX;
	}
	return (uint64_t)v32;
}

// オフセット設定
void vmem_set_offset(int32_t offset) {
	vmem_offset = offset;
}

// リセット
void vmem_reset(void) {
	vmem_offset = 0;
}

// モード設定
void vmem_set_mode(vmem_mode_t mode) {
	current_mode = mode;
}

// 物理→仮想変換関数の設定
void vmem_set_phys2virt(vmem_phys2virt_fn fn) {
	if (fn)
		phys2virt = fn;
	else
		phys2virt = default_phys2virt;
}
