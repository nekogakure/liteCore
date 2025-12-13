#include <util/config.h>
#include <mem/manager.h>
#include <util/io.h>
#include <mem/map.h>
#include <mem/vmem.h>
#include <util/console.h>
#include <interrupt/irq.h>
#include <sync/spinlock.h>
#include <stdint.h>

// ブロックヘッダは8バイト境界
typedef struct block_header {
	uint32_t size;
	uint32_t tag;
	struct block_header *next;
} block_header_t;

#define KMALLOC_CANARY 0xDEADBEEF

// ヒープの先頭とフリーリストのヘッド
static block_header_t *free_list = NULL;
static uintptr_t heap_start_addr = 0;
static uintptr_t heap_end_addr = 0;
static spinlock_t heap_lock = { 0 };

static uint32_t alloc_seq = 1;

#define ALIGN 8

static int heap_expand(uint32_t additional_size);
static void *kmalloc_internal(uint32_t size, int retry_count);

// sizeをALIGNに丸める（ヘッダを除くユーザ領域のサイズ）
static inline uint32_t align_up(uint32_t size) {
	return (size + (ALIGN - 1)) & ~(ALIGN - 1);
}

/**
 * @fn mem_init
 * @brief メモリを初期化します
 *
 * @param start ヒープ領域の開始アドレス
 * @param end   ヒープ領域の終了アドレス
 */
void mem_init(uint32_t start, uint32_t end) {
	if (end <= start || (end - start) < sizeof(block_header_t)) {
		return;
	}

	heap_start_addr = start;
	heap_end_addr = end;

	// 最初のフリーブロック
	free_list = (block_header_t *)(uintptr_t)start;
	free_list->size = end - start;
	free_list->next = NULL;
#ifdef INIT_MSG
	printk("Memory initialized: heap %x - %x (size=%u)\n",
	       (unsigned int)start, (unsigned int)end,
	       (unsigned int)(end - start));
#endif
}

/**
 * @fn kmalloc
 * @brief 指定したサイズのメモリを確保します
 *
 * @param size 確保するバイト数
 * @return 確保したメモリ領域へのポインタ。失敗時はNULL
 */
void *kmalloc(uint32_t size) {
	return kmalloc_internal(size, 0);
}

static void *kmalloc_internal(uint32_t size, int retry_count) {
	if (size == 0 || free_list == NULL) {
		return NULL;
	}

	/* Prevent infinite recursion */
	if (retry_count > 3) {
		printk("mem: kmalloc retry limit exceeded\n");
		return NULL;
	}

	uint32_t flags = 0;
	spin_lock_irqsave(&heap_lock, &flags);

	uint32_t wanted = align_up(size);

	/* Add space for canary at the end, then align the total */
	uint32_t wanted_with_canary = align_up(wanted + sizeof(uint32_t));

	// ブロック全体の必要サイズ (header + user data + canary, aligned)
	uint32_t total_size = wanted_with_canary + sizeof(block_header_t);

	block_header_t *prev = NULL;
	block_header_t *cur = free_list;

	/* Debug: trace free list for large allocations (commented out for now)
	if (size >= 4096) {
		printk("mem: kmalloc(%u) retry=%d searching free_list:\n", size, retry_count);
		block_header_t *dbg = free_list;
		int count = 0;
		while (dbg && count < 10) {
			printk("  block[%d]: addr=0x%08x size=%u next=0x%08x\n",
			       count, (uint32_t)dbg, dbg->size, (uint32_t)dbg->next);
			dbg = dbg->next;
			count++;
		}
		if (dbg) {
			printk("  ... (more blocks)\n");
		}
	}
	*/

	while (cur) {
		/* Skip and remove zero-sized blocks from free list */
		if (cur->size == 0) {
			printk("mem: WARNING: zero-sized block found at 0x%p, removing from free list\n",
			       cur);
			/* Remove from list */
			if (prev) {
				prev->next = cur->next;
			} else {
				free_list = cur->next;
			}
			cur = cur->next;
			continue;
		}

		if (cur->size >= total_size) {
			/* Only split if remainder is useful (header + at least ALIGN bytes for user) */
			if (cur->size >=
			    total_size + sizeof(block_header_t) + ALIGN * 2) {
				// 分割可能 -> 残りを新しいフリーブロックにする
				uintptr_t cur_addr = (uintptr_t)cur;
				block_header_t *next_block =
					(block_header_t *)(cur_addr +
							   total_size);
				next_block->size = cur->size - total_size;
				next_block->next = cur->next;

				// 現在のブロックを返却対象としてサイズを調整
				cur->size = total_size;

				if (prev) {
					prev->next = next_block;
				} else {
					free_list = next_block;
				}
			} else {
				// 分割しないで全体を返す
				if (prev) {
					prev->next = cur->next;
				} else {
					free_list = cur->next;
				}
			}

			// ユーザ領域はヘッダの直後
			void *user_ptr = (void *)((uintptr_t)cur +
						  sizeof(block_header_t));

			uint32_t total_free = 0;
			uint32_t largest = 0;
			block_header_t *it = free_list;
			while (it) {
				if (it->size > sizeof(block_header_t)) {
					uint32_t user_bytes =
						it->size -
						sizeof(block_header_t);
					total_free += user_bytes;
					if (user_bytes > largest)
						largest = user_bytes;
				}
				it = it->next;
			}
			/* Debug logging for larger allocations to trace fragmentation */
			if (size >= 256) {
				((block_header_t *)cur)->tag = alloc_seq++;
			}

			/* Place canary at end of allocated space (before alignment padding) */
			uint32_t *canary = (uint32_t *)((uintptr_t)user_ptr +
							wanted_with_canary -
							sizeof(uint32_t));
			*canary = KMALLOC_CANARY;

			spin_unlock_irqrestore(&heap_lock, flags);
			return user_ptr;
		}

		prev = cur;
		cur = cur->next;
	}

	// 見つからなかった - ヒープを拡張してリトライ
	spin_unlock_irqrestore(&heap_lock, flags);

	uint32_t expand_size = total_size;
	if (expand_size < 0x100000) /* 1MB minimum */
		expand_size = 0x100000;

	if (heap_expand(expand_size) == 0) {
		/* Retry allocation after expansion */
		return kmalloc_internal(size, retry_count + 1);
	}

	printk("mem: heap expansion failed, allocation failed\n");
	return NULL;
}

/**
 * @fn kfree
 * @brief メモリを解放します
 *
 * @param ptr 解放するメモリ領域へのポインタ
 */
void kfree(void *ptr) {
	if (ptr == NULL) {
		return;
	}

	uint32_t flags = 0;
	spin_lock_irqsave(&heap_lock, &flags);

	// ヘッダはユーザポインタの前にある
	block_header_t *hdr =
		(block_header_t *)((uintptr_t)ptr - sizeof(block_header_t));

	// 基本的なサニティチェック: 最小限のヒープ開始アドレスのみチェック
	// 動的拡張により heap_end_addr は信頼できないため、上限チェックは省略
	uintptr_t hdr_addr = (uintptr_t)hdr;
	if (hdr_addr < heap_start_addr) {
		// 明らかに範囲外のポインタは無視
		spin_unlock_irqrestore(&heap_lock, flags);
		return;
	}

	// フリーリストに挿入（アドレス順に保つ）
	/* Check canary: it's placed at user_area_end - 4 bytes */
	if (hdr->size > sizeof(block_header_t) + sizeof(uint32_t)) {
		uint32_t user_bytes_with_canary =
			hdr->size - sizeof(block_header_t);
		uint32_t *canary =
			(uint32_t *)((uintptr_t)hdr + sizeof(block_header_t) +
				     user_bytes_with_canary - sizeof(uint32_t));
		if (*canary != KMALLOC_CANARY) {
			uint32_t got = *canary;
			uint32_t tag = hdr->tag;
			printk("mem: kfree CANARY MISMATCH for ptr=%p hdr=%p hdr->size=%u id=%u expected=0x%08x got=0x%08x\n",
			       ptr, hdr, hdr->size, tag,
			       (unsigned)KMALLOC_CANARY, (unsigned)got);

			uint8_t *user_ptr =
				(uint8_t *)hdr + sizeof(block_header_t);
			uint8_t *ctx_start =
				user_ptr +
				(user_bytes_with_canary > 16 ?
					 user_bytes_with_canary - 16 :
					 0);
			uint32_t ctx_len = (user_bytes_with_canary > 16) ?
						   24 :
						   (user_bytes_with_canary + 8);
			if (ctx_len > user_bytes_with_canary + 8)
				ctx_len = user_bytes_with_canary + 8;
			if (ctx_len > 64)
				ctx_len = 64;
			printk("mem: dumping %u bytes around canary (hex): ",
			       ctx_len);
			for (uint32_t i = 0; i < ctx_len; ++i) {
				uint8_t v = ctx_start[i];
				printk("%02x", v);
				if ((i & 0xF) == 0xF)
					printk(" ");
			}
			printk("\n");
		}
	}

	if (free_list == NULL || hdr < free_list) {
		hdr->next = free_list;
		free_list = hdr;
	} else {
		block_header_t *cur = free_list;
		while (cur->next && cur->next < hdr) {
			cur = cur->next;
		}
		hdr->next = cur->next;
		cur->next = hdr;
	}

	block_header_t *cur = free_list;
	while (cur && cur->next) {
		uintptr_t cur_end = (uintptr_t)cur + cur->size;
		uintptr_t next_addr = (uintptr_t)cur->next;
		if (cur_end == next_addr) {
			// 連続している -> 併合
			cur->size += cur->next->size;
			cur->next = cur->next->next;
			// 続けて併合の可能性があるのでループを継続
		} else {
			cur = cur->next;
		}
	}

	spin_unlock_irqrestore(&heap_lock, flags);
}

/**
 * @fn heap_expand
 * @brief ヒープを拡張します
 *
 * @param additional_size 追加するバイト数
 * @return 成功時は0、失敗時は-1
 */
static int heap_expand(uint32_t additional_size) {
	if (additional_size == 0)
		return 0;

	/* Align to page boundary */
	additional_size = (additional_size + 0x0FFF) & ~0x0FFF;

	/* CRITICAL: Do NOT call alloc_frame() here as it triggers kmalloc() recursion!
	 * Instead, directly use the memory at heap_end_addr (identity-mapped).
	 * Physical frame reservation can be done separately if needed.
	 */

	uintptr_t new_block_addr = heap_end_addr;

	printk("mem: heap_expand creating block at 0x%08x size=%u (direct allocation, no frame alloc)\n",
	       (uint32_t)new_block_addr, additional_size);

	uint32_t flags = 0;
	spin_lock_irqsave(&heap_lock, &flags);

	/* Create a new free block at the heap end */
	block_header_t *new_block = (block_header_t *)new_block_addr;
	new_block->size = additional_size;
	new_block->next = NULL;
	new_block->tag = 0;

	/* Add to free list in address-sorted order */
	if (free_list == NULL) {
		free_list = new_block;
		printk("mem: heap_expand set as first free block\n");
	} else {
		block_header_t *prev = NULL;
		block_header_t *cur = free_list;

		/* Find insertion point to maintain address order */
		while (cur && (uintptr_t)cur < (uintptr_t)new_block) {
			prev = cur;
			cur = cur->next;
		}

		if (prev == NULL) {
			/* Insert at head */
			new_block->next = free_list;
			free_list = new_block;
			printk("mem: heap_expand inserted at head\n");
		} else {
			/* Insert after prev */
			new_block->next = prev->next;
			prev->next = new_block;
			printk("mem: heap_expand inserted after 0x%p\n", prev);

			/* Try to merge with previous block if adjacent */
			uintptr_t prev_end = (uintptr_t)prev + prev->size;
			if (prev_end == (uintptr_t)new_block) {
				printk("mem: heap_expand merging with previous block\n");
				prev->size += new_block->size;
				prev->next = new_block->next;
				new_block =
					prev; /* For potential merge with next */
			}
		}

		/* Try to merge with next block if adjacent */
		if (new_block->next) {
			uintptr_t new_end =
				(uintptr_t)new_block + new_block->size;
			if (new_end == (uintptr_t)new_block->next) {
				printk("mem: heap_expand merging with next block\n");
				block_header_t *next = new_block->next;
				new_block->size += next->size;
				new_block->next = next->next;
			}
		}
	}

	heap_end_addr += additional_size;

	/* Debug: verify free list after expansion (commented out for now)
	printk("mem: free_list after expansion (first 3):\n");
	block_header_t *dbg = free_list;
	int count = 0;
	while (dbg && count < 3) {
		printk("  [%d] addr=0x%p size=%u next=0x%p\n",
		       count, dbg, dbg->size, dbg->next);
		dbg = dbg->next;
		count++;
	}
	*/

	spin_unlock_irqrestore(&heap_lock, flags);

	printk("mem: heap expanded by %u bytes, new heap_end=0x%08x\n",
	       additional_size, (uint32_t)heap_end_addr);

	return 0;
}

/**
 * @fn mem_has_space
 * @brief 指定したメモリタイプでsizeバイト分の空きがあるか判定する
 *
 * - MEM_TYPE_HEAP: 連続するsizeバイトを割当可能なフリーブロックが存在するか
 * - MEM_TYPE_FRAME: 連続するceil(size/FRAME_SIZE)フレームが存在するか
 */
int mem_has_space(mem_type_t type, uint32_t size) {
	if (type == MEM_TYPE_HEAP) {
		// 連続領域が必要なので、フリーリスト上にsizeバイト以上のブロックがあるか探す
		uint32_t flags = 0;
		spin_lock_irqsave(&heap_lock, &flags);
		uint32_t wanted = align_up(size);
		block_header_t *cur = free_list;
		while (cur) {
			if (cur->size >= wanted + sizeof(block_header_t)) {
				spin_unlock_irqrestore(&heap_lock, flags);
				return 1;
			}
			cur = cur->next;
		}
		spin_unlock_irqrestore(&heap_lock, flags);
		return 0;
	} else if (type == MEM_TYPE_FRAME) {
		// 必要なフレーム数を計算し、memmapのビットマップ上で連続する空きフレームを探す
		const memmap_t *mm = memmap_get();
		if (!mm || mm->frames == 0)
			return 0;

		uint32_t need_frames = (size + FRAME_SIZE - 1) / FRAME_SIZE;
		if (need_frames == 0)
			need_frames = 1;

		uint32_t consecutive = 0;
		for (uint32_t i = 0; i < mm->frames; ++i) {
			// ビットが0なら空き
			uint32_t word = mm->bitmap[i / 32];
			uint32_t bit = (word >> (i % 32)) & 1u;
			if (bit == 0) {
				consecutive++;
				if (consecutive >= need_frames)
					return 1;
			} else {
				consecutive = 0;
			}
		}
		return 0;
	}

	return 0; // なんやこれ知らんぞ用
}

/**
 * @fn memory_init
 * @brief メモリマップなどを初期化します
 */
void memory_init() {
	/* Expand managed physical memory range to cover more guest RAM.
	 * Previous value was 1MB - 5MB (4MB). Increase to 1MB - 64MB
	 * so the kernel can allocate more frames and provide a larger heap.
	 */
	/* initialize memmap for physical range 1MB - 64MB */
	memmap_init(0x100000ULL, 0x4000000ULL); // 1MB - 64MB

	extern uint32_t __end;
	const memmap_t *mm = memmap_get();
	uintptr_t base_end = (uintptr_t)&__end;
	uintptr_t bitmap_end = base_end;
	// mm、mm->bitmap、mm->max_framesが有効か確認
	if (mm && mm->bitmap && mm->max_frames) {
		// bitmapはmemmap内でバイト配列として格納されているので、その終了アドレスを計算
		uint32_t bitmap_bytes = (mm->max_frames + 7) / 8;
		bitmap_end = (uintptr_t)mm->bitmap + bitmap_bytes;
	}
	// kernelの終了アドレスとbitmapの終了アドレスの大きい方を選択
	uintptr_t heap_start = (base_end > bitmap_end) ? base_end : bitmap_end;
	// 4KBページ境界に切り上げてアライン
	heap_start = (heap_start + 0x0FFF) & ~0x0FFF;

	/* Start with 2MB heap, will expand dynamically as needed */
	uintptr_t heap_end = heap_start + 0x200000; // 2MBの初期ヒープ領域
	mem_init((uint32_t)heap_start, (uint32_t)heap_end);

	/* Reserve the physical frames backing the heap. The computed heap_start
	 * is a virtual/kernel address; memmap_reserve expects physical addresses.
	 * Convert via vmem_virt_to_phys()/vmem_virt_to_phys64 and fall back to the
	 * original values if conversion fails (best-effort).
	 */
	uint64_t phys_start = 0;
	uint64_t phys_end = 0;
	/* try 32-bit conversion first (common case) */
	uint32_t p32 = vmem_virt_to_phys((uint32_t)heap_start);
	if (p32 != 0) {
		phys_start = (uint64_t)p32;
		uint32_t p32_end = vmem_virt_to_phys((uint32_t)(heap_end - 1));
		if (p32_end != 0) {
			phys_end = (uint64_t)p32_end + 1;
		}
	}
	if (phys_start == 0 || phys_end == 0) {
		/* try 64-bit walker if 32-bit helper failed */
		uint64_t p64_start = vmem_virt_to_phys64((uint64_t)heap_start);
		uint64_t p64_end =
			vmem_virt_to_phys64((uint64_t)(heap_end - 1));
		if (p64_start != 0 && p64_end != 0 && p64_end >= p64_start) {
			phys_start = p64_start;
			phys_end = p64_end + 1;
		}
	}
	if (phys_start == 0 || phys_end == 0) {
		/* fallback: use virtual as-is (legacy behavior) but log clearly */
		phys_start = (uint64_t)heap_start;
		phys_end = (uint64_t)heap_end;
		printk("mem: WARNING memmap_reserve using virtual addresses as-phys start=0x%08x end=0x%08x\n",
		       (unsigned)heap_start, (unsigned)heap_end);
	}
	memmap_reserve(phys_start, phys_end);
}

/**
 * @fn stack_alloc
 * @brief 下方向に伸びるカーネルスタック領域を確保する
 * @param size 要求サイズ（バイト）。内部で ALIGN に丸められる。
 * @return スタックのトップ（高位アドレス）。失敗時はNULL。
 */
void *stack_alloc(uint32_t size) {
	if (size == 0)
		return NULL;
	uint32_t wanted = align_up(size);
	void *p = kmalloc(wanted);
	if (!p)
		return NULL;
	return (void *)((uintptr_t)p + wanted);
}

/**
 * @fn stack_free
 * @brief stack_alloc で確保したスタックを解放する
 * @param top stack_alloc が返したトップアドレス
 * @param size 元の要求サイズ
 */
void stack_free(void *top, uint32_t size) {
	if (!top || size == 0)
		return;
	uint32_t wanted = align_up(size);
	/* top は p + wanted なので p = top - wanted */
	uintptr_t p = (uintptr_t)top - wanted;
	kfree((void *)p);
}

/* Heap statistics helpers (外部から利用可能にする) */
uint32_t heap_total_bytes(void) {
	if (heap_end_addr > heap_start_addr) {
		return heap_end_addr - heap_start_addr;
	}
	return 0;
}

uint32_t heap_free_bytes(void) {
	uint32_t total_free = 0;
	uint32_t flags = 0;
	spin_lock_irqsave(&heap_lock, &flags);
	block_header_t *cur = free_list;
	while (cur) {
		uint32_t user_bytes = 0;
		if (cur->size > sizeof(block_header_t)) {
			user_bytes = cur->size - sizeof(block_header_t);
		}
		total_free += user_bytes;
		cur = cur->next;
	}
	spin_unlock_irqrestore(&heap_lock, flags);
	return total_free;
}

uint32_t heap_largest_free_block(void) {
	uint32_t largest = 0;
	uint32_t flags = 0;
	spin_lock_irqsave(&heap_lock, &flags);
	block_header_t *cur = free_list;
	while (cur) {
		if (cur->size > sizeof(block_header_t)) {
			uint32_t user_bytes =
				cur->size - sizeof(block_header_t);
			if (user_bytes > largest)
				largest = user_bytes;
		}
		cur = cur->next;
	}
	spin_unlock_irqrestore(&heap_lock, flags);
	return largest;
}
