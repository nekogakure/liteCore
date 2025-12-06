/**
 * @file map.c
 * @brief ビットマップ方式のフレームアロケータ（シンプル実装）
 */

#include <util/config.h>
#include <mem/map.h>
#include <util/console.h>
#include <util/io.h>
#include <mem/manager.h>

#define CHUNK_SIZE (1ULL << 20) /* 1 MiB per chunk */

/* protect bitmap operations */
static volatile uint32_t memmap_lock_storage = 0;

// 管理情報をまとめた構造体
static memmap_t memmap = { 0 };

typedef struct chunk {
	uint64_t idx; // chunk index relative to memmap.start_frame
	uint32_t *words; // pointer to bitmap words (allocated with kmalloc)
	struct chunk *next;
} chunk_t;

static chunk_t *chunk_head = NULL;

static inline uint64_t frames_per_chunk(void) {
	return CHUNK_SIZE / FRAME_SIZE;
}

static inline uint32_t chunk_word_count(void) {
	uint64_t fpc = frames_per_chunk();
	return (uint32_t)((fpc + 31) / 32);
}

/* find chunk by index */
static chunk_t *find_chunk(uint64_t idx) {
	chunk_t *c = chunk_head;
	while (c) {
		if (c->idx == idx)
			return c;
		c = c->next;
	}
	return NULL;
}

/* create chunk (allocate bitmap words and insert into list) */
static chunk_t *create_chunk(uint64_t idx) {
	chunk_t *c = (chunk_t *)kmalloc(sizeof(chunk_t));
	if (!c)
		return NULL;
	uint32_t words = chunk_word_count();
	uint32_t *w = (uint32_t *)kmalloc(words * sizeof(uint32_t));
	if (!w) {
		kfree(c);
		return NULL;
	}
	for (uint32_t i = 0; i < words; ++i)
		w[i] = 0; /* free=0 */
	c->idx = idx;
	c->words = w;
	c->next = chunk_head;
	chunk_head = c;
	return c;
}

/* chunk bitmap ops */
static inline int chunk_test(chunk_t *c, uint64_t local_idx) {
	uint32_t w = (uint32_t)(local_idx / 32);
	uint32_t b = (uint32_t)(local_idx % 32);
	return (c->words[w] >> b) & 1u;
}

static inline void chunk_set(chunk_t *c, uint64_t local_idx) {
	uint32_t w = (uint32_t)(local_idx / 32);
	uint32_t b = (uint32_t)(local_idx % 32);
	c->words[w] |= (1u << b);
}

static inline void chunk_clear(chunk_t *c, uint64_t local_idx) {
	uint32_t w = (uint32_t)(local_idx / 32);
	uint32_t b = (uint32_t)(local_idx % 32);
	c->words[w] &= ~(1u << b);
}

/**
 * @fn memmap_init
 * @brief 指定された範囲の物理メモリフレームを管理対象として初期化する
 * @param start 管理開始アドレス（物理アドレス）
 * @param end 管理終了アドレス（物理アドレス、end-1まで管理）
 */
void memmap_init(uint64_t start, uint64_t end) {
	// フレーム数計算
	uint64_t start_frame = start / FRAME_SIZE;
	uint64_t end_frame = (end + FRAME_SIZE - 1) / FRAME_SIZE; // round up
	uint64_t count = 0;

	if (end <= start) {
		return;
	}

	count = end_frame - start_frame;

	// memmap構造体へ設定（チャンク方式では bitmap を使わない）
	memmap.start_addr = start;
	memmap.end_addr = end;
	memmap.start_frame = start_frame;
	memmap.frames = count;
	memmap.max_frames = count; /* logical max */
	memmap.bitmap = NULL;

	/* Diagnostic: print memmap summary so we can see physical ranges managed */
	printk("memmap: init start_addr=0x%08llx end_addr=0x%08llx start_frame=%llu frames=%llu\n",
	       (unsigned long long)memmap.start_addr,
	       (unsigned long long)memmap.end_addr,
	       (unsigned long long)memmap.start_frame,
	       (unsigned long long)memmap.frames);

	/* clear any existing chunk list */
	{
		uint32_t flags = 0;
		extern void spin_lock_irqsave(volatile uint32_t *lock,
					      uint32_t *flagsptr);
		extern void spin_unlock_irqrestore(volatile uint32_t *lock,
						   uint32_t flags);
		spin_lock_irqsave(&memmap_lock_storage, &flags);
		chunk_t *c = chunk_head;
		while (c) {
			chunk_t *n = c->next;
			if (c->words)
				kfree(c->words);
			kfree(c);
			c = n;
		}
		chunk_head = NULL;
		spin_unlock_irqrestore(&memmap_lock_storage, flags);
	}
#ifdef INIT_MSG
	printk("MemoryMap initialized: frames=%llu start_frame=%llu\n",
	       (unsigned long long)memmap.frames,
	       (unsigned long long)memmap.start_frame);
#endif
}

/**
 * @fn alloc_frame
 * @brief 空いているフレームを1つ割り当てて、そのアドレスを返す
 * @return 割り当てたフレームのアドレス。空きがなければNULL
 */
void *alloc_frame(void) {
	if (memmap.frames == 0) {
		printk("alloc_frame: memmap not initialized\n");
		return NULL;
	}

	uint32_t flags = 0;
	extern void spin_lock_irqsave(volatile uint32_t *lock,
				      uint32_t *flagsptr);
	extern void spin_unlock_irqrestore(volatile uint32_t *lock,
					   uint32_t flags);
	spin_lock_irqsave(&memmap_lock_storage, &flags);

	uint64_t fpc = frames_per_chunk();
	uint64_t max_frames = memmap.frames;
	uint64_t max_chunk = (max_frames + fpc - 1) / fpc;

	for (uint64_t chi = 0; chi < max_chunk; ++chi) {
		chunk_t *c = find_chunk(chi);
		if (!c) {
			/* lazily create chunk */
			c = create_chunk(chi);
			if (!c)
				continue; /* try next chunk */
		}
		/* scan words for a zero bit */
		uint32_t words = chunk_word_count();
		for (uint32_t w = 0; w < words; ++w) {
			if (c->words[w] != 0xFFFFFFFFu) {
				/* find free bit */
				for (uint32_t b = 0; b < 32; ++b) {
					uint64_t bit_idx = (uint64_t)w * 32 + b;
					if (bit_idx >= fpc)
						break;
					if (!chunk_test(c, bit_idx)) {
						chunk_set(c, bit_idx);
						uint64_t frame_no =
							memmap.start_frame +
							chi * fpc + bit_idx;
						if (frame_no >=
						    memmap.start_frame +
							    memmap.frames) {
							/* out of managed range */
							chunk_clear(c, bit_idx);
							spin_unlock_irqrestore(
								&memmap_lock_storage,
								flags);
							return NULL;
						}
						void *addr =
							(void *)(uintptr_t)(frame_no *
									    FRAME_SIZE);
						spin_unlock_irqrestore(
							&memmap_lock_storage,
							flags);
						return addr;
					}
				}
			}
		}
	}

	spin_unlock_irqrestore(&memmap_lock_storage, flags);
	return NULL; /* no free frames */
}

/**
 * @fn free_frame
 * @brief 指定したフレームアドレスを解放する
 * @param addr 解放するフレームのアドレス
 */
void free_frame(void *addr) {
	if (addr == NULL) {
		return;
	}

	uintptr_t a = (uintptr_t)addr;
	if (a % FRAME_SIZE != 0) {
		printk("MemoryMap: border is invalid: %lx", (unsigned long)a);
		return;
	}

	uint64_t frame_no = a / FRAME_SIZE;
	if (frame_no < memmap.start_frame) {
		return;
	}

	uint64_t idx = frame_no - memmap.start_frame;
	if (idx >= memmap.frames) {
		return;
	}

	uint64_t fpc = frames_per_chunk();
	uint64_t chi = idx / fpc;
	uint64_t local = idx % fpc;

	uint32_t flags = 0;
	extern void spin_lock_irqsave(volatile uint32_t *lock,
				      uint32_t *flagsptr);
	extern void spin_unlock_irqrestore(volatile uint32_t *lock,
					   uint32_t flags);
	spin_lock_irqsave(&memmap_lock_storage, &flags);
	chunk_t *c = find_chunk(chi);
	if (c) {
		chunk_clear(c, local);
		/* Note: we do not free empty chunk structures in this PoC */
	}
	spin_unlock_irqrestore(&memmap_lock_storage, flags);
}

/**
 * @fn frame_count
 * @brief 管理しているフレーム数を返す
 * @return 管理しているフレーム数
 */
uint64_t frame_count(void) {
	uint64_t f = 0;
	uint32_t flags = 0;
	extern void spin_lock_irqsave(volatile uint32_t *lock,
				      uint32_t *flagsptr);
	extern void spin_unlock_irqrestore(volatile uint32_t *lock,
					   uint32_t flags);
	spin_lock_irqsave(&memmap_lock_storage, &flags);
	f = memmap.frames;
	spin_unlock_irqrestore(&memmap_lock_storage, flags);
	return f;
}

/**
 * @fn memmap_reserve
 * @brief メモリ領域を予約する
 * @param start 予約する領域の開始地点
 * @param end 予約する領域の終了地点
 */
void memmap_reserve(uint64_t start, uint64_t end) {
	if (memmap.frames == 0)
		return;

	printk("memmap_reserve: request start=0x%08llx end=0x%08llx\n",
	       (unsigned long long)start, (unsigned long long)end);

	uint64_t start_frame = start / FRAME_SIZE;
	uint64_t end_frame = (end + FRAME_SIZE - 1) / FRAME_SIZE;

	if (end_frame <= memmap.start_frame)
		return;
	if (start_frame >= memmap.start_frame + memmap.frames)
		return;

	uint64_t s = (start_frame < memmap.start_frame) ?
			     0 :
			     (start_frame - memmap.start_frame);
	uint64_t e = (end_frame > memmap.start_frame + memmap.frames) ?
			     memmap.frames :
			     (end_frame - memmap.start_frame);

	uint64_t fpc = frames_per_chunk();

	uint32_t flags = 0;
	extern void spin_lock_irqsave(volatile uint32_t *lock,
				      uint32_t *flagsptr);
	extern void spin_unlock_irqrestore(volatile uint32_t *lock,
					   uint32_t flags);
	spin_lock_irqsave(&memmap_lock_storage, &flags);
	for (uint64_t idx = s; idx < e; ++idx) {
		uint64_t chi = idx / fpc;
		uint64_t local = idx % fpc;
		chunk_t *c = find_chunk(chi);
		if (!c) {
			c = create_chunk(chi);
			if (!c)
				continue; /* best effort */
		}
		chunk_set(c, local);
	}
	spin_unlock_irqrestore(&memmap_lock_storage, flags);
}

/**
 * @fn memmap_get
 * @brief メモリマップ構造体を返す（だけ）
 */
const memmap_t *memmap_get(void) {
	return &memmap;
}
