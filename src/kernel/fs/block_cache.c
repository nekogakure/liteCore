#include <fs/block_cache.h>
#include <driver/ata.h>
#include <mem/manager.h>
#include <util/console.h>

/**
 * @brief LRUアルゴリズムで最も古いエントリを見つける
 */
static struct block_cache_entry *find_lru_entry(struct block_cache *cache) {
	struct block_cache_entry *lru = NULL;
	uint32_t oldest_time = 0xFFFFFFFF;

	for (uint32_t i = 0; i < cache->num_entries; i++) {
		struct block_cache_entry *e = &cache->entries[i];

		/* 無効なエントリがあれば優先的に使う */
		if (!e->valid) {
			return e;
		}

		/* 最も古いエントリを探す */
		if (e->last_used < oldest_time) {
			oldest_time = e->last_used;
			lru = e;
		}
	}

	return lru;
}

/**
 * @brief ブロックをATAから読み込む
 */
static int read_block_from_disk(struct block_cache *cache, uint32_t block_num,
				void *buffer) {
	/* ブロック番号をセクタ番号に変換 */
	uint32_t sectors_per_block = cache->block_size / 512;
	uint32_t start_sector = block_num * sectors_per_block;

	uint32_t total_read = 0;
	while (total_read < sectors_per_block) {
		uint8_t count = (sectors_per_block - total_read > 255) ?
					255 :
					(sectors_per_block - total_read);

		int r = ata_read_sectors(cache->drive,
					 start_sector + total_read, count,
					 (uint8_t *)buffer + total_read * 512);
		if (r != 0) {
			printk("BlockCache: read_block_from_disk failed at sector %u (drive=%u)\n",
			       start_sector + total_read, cache->drive);
			return -1;
		}

		total_read += count;
	}

	return 0;
}

/**
 * @brief ブロックをATAに書き込む
 */
static int write_block_to_disk(struct block_cache *cache, uint32_t block_num,
			       const void *buffer) {
	/* ブロック番号をセクタ番号に変換 */
	uint32_t sectors_per_block = cache->block_size / 512;
	uint32_t start_sector = block_num * sectors_per_block;

	/* ATAに書き込む（最大255セクタまで一度に書ける） */
	uint32_t total_written = 0;
	while (total_written < sectors_per_block) {
		uint8_t count = (sectors_per_block - total_written > 255) ?
					255 :
					(sectors_per_block - total_written);

		int r = ata_write_sectors(
			cache->drive, start_sector + total_written, count,
			(const uint8_t *)buffer + total_written * 512);
		if (r != 0) {
			return -1;
		}

		total_written += count;
	}

	return 0;
}

/**
 * @brief ブロックキャッシュを初期化する
 */
struct block_cache *block_cache_init(uint8_t drive, uint32_t block_size,
				     uint32_t num_entries) {
	struct block_cache *cache =
		(struct block_cache *)kmalloc(sizeof(struct block_cache));
	if (!cache) {
		return NULL;
	}

	cache->drive = drive;
	cache->block_size = block_size;
	cache->num_entries = num_entries;
	cache->timestamp = 0;
	cache->hits = 0;
	cache->misses = 0;

	/* エントリ配列を確保 */
	cache->entries = (struct block_cache_entry *)kmalloc(
		sizeof(struct block_cache_entry) * num_entries);
	if (!cache->entries) {
		kfree(cache);
		return NULL;
	}

	/* 各エントリを初期化 */
	for (uint32_t i = 0; i < num_entries; i++) {
		struct block_cache_entry *e = &cache->entries[i];
		e->block_num = 0;
		e->last_used = 0;
		e->dirty = 0;
		e->valid = 0;

		/* ブロックデータ用のバッファを確保 */
		e->data = (uint8_t *)kmalloc(block_size + 4);
		if (!e->data) {
			/* 確保失敗時はクリーンアップ */
			for (uint32_t j = 0; j < i; j++) {
				kfree(cache->entries[j].data);
			}
			kfree(cache->entries);
			kfree(cache);
			return NULL;
		}
	}

	return cache;
}

/**
 * @brief ブロックを読み取る（キャッシュ経由）
 */
int block_cache_read(struct block_cache *cache, uint32_t block_num,
		     void *buffer) {
	if (!cache || !buffer) {
		return -1;
	}

	/* タイムスタンプを進める */
	cache->timestamp++;

	/* キャッシュを検索 */
	for (uint32_t i = 0; i < cache->num_entries; i++) {
		struct block_cache_entry *e = &cache->entries[i];

		if (e->valid && e->block_num == block_num) {
			/* キャッシュヒット */
			cache->hits++;
			e->last_used = cache->timestamp;

			/* データをコピー */
			for (uint32_t j = 0; j < cache->block_size; j++) {
				((uint8_t *)buffer)[j] = e->data[j];
			}

			return 0;
		}
	}

	/* キャッシュミス */
	cache->misses++;

	/* LRUエントリを見つける */
	struct block_cache_entry *lru = find_lru_entry(cache);
	if (!lru) {
		printk("BlockCache: no LRU entry available (num_entries=%u)\n",
		       cache->num_entries);
		return -1;
	}

	/* dirtyエントリの場合はディスクに書き戻す */
	if (lru->valid && lru->dirty) {
		if (write_block_to_disk(cache, lru->block_num, lru->data) !=
		    0) {
			return -1;
		}
		lru->dirty = 0;
	}

	/* ディスクから読み込む */
	if (read_block_from_disk(cache, block_num, lru->data) != 0) {
		/* Diagnostic: report block read failure */
		printk("BlockCache: failed to read block %u (block_size=%u entries=%u drive=%u)\n",
		       block_num, cache->block_size, cache->num_entries,
		       cache->drive);
		return -1;
	}

	/* エントリを更新 */
	lru->block_num = block_num;
	lru->last_used = cache->timestamp;
	lru->valid = 1;
	lru->dirty = 0;

	/* データをコピー */
	for (uint32_t j = 0; j < cache->block_size; j++) {
		((uint8_t *)buffer)[j] = lru->data[j];
	}

	return 0;
}

/**
 * @brief ブロックを書き込む（キャッシュ経由）
 */
int block_cache_write(struct block_cache *cache, uint32_t block_num,
		      const void *buffer) {
	if (!cache || !buffer) {
		return -1;
	}

	/* タイムスタンプを進める */
	cache->timestamp++;

	/* キャッシュを検索 */
	for (uint32_t i = 0; i < cache->num_entries; i++) {
		struct block_cache_entry *e = &cache->entries[i];

		if (e->valid && e->block_num == block_num) {
			/* キャッシュヒット：データを更新 */
			for (uint32_t j = 0; j < cache->block_size; j++) {
				e->data[j] = ((const uint8_t *)buffer)[j];
			}
			e->last_used = cache->timestamp;
			e->dirty = 1;
			return 0;
		}
	}

	/* キャッシュミス：新しいエントリを作成 */
	struct block_cache_entry *lru = find_lru_entry(cache);
	if (!lru) {
		return -1;
	}

	/* dirtyエントリの場合はディスクに書き戻す */
	if (lru->valid && lru->dirty) {
		if (write_block_to_disk(cache, lru->block_num, lru->data) !=
		    0) {
			return -1;
		}
	}

	/* データをコピー */
	for (uint32_t j = 0; j < cache->block_size; j++) {
		lru->data[j] = ((const uint8_t *)buffer)[j];
	}

	/* エントリを更新 */
	lru->block_num = block_num;
	lru->last_used = cache->timestamp;
	lru->valid = 1;
	lru->dirty = 1;

	return 0;
}

/**
 * @brief dirtyブロックをすべてディスクに書き込む
 */
int block_cache_flush(struct block_cache *cache) {
	if (!cache) {
		return -1;
	}

	for (uint32_t i = 0; i < cache->num_entries; i++) {
		struct block_cache_entry *e = &cache->entries[i];

		if (e->valid && e->dirty) {
			if (write_block_to_disk(cache, e->block_num, e->data) !=
			    0) {
				return -1;
			}
			e->dirty = 0;
		}
	}

	return 0;
}

/**
 * @brief キャッシュ統計情報を表示する
 */
void block_cache_print_stats(struct block_cache *cache) {
	if (!cache) {
		return;
	}

	uint32_t total = cache->hits + cache->misses;
	uint32_t hit_rate = (total > 0) ? (cache->hits * 100 / total) : 0;

	printk("Block Cache Statistics:\n");
	printk("  Entries: %u x %u bytes = %u KB\n", cache->num_entries,
	       cache->block_size,
	       (cache->num_entries * cache->block_size) / 1024);
	printk("  Hits: %u\n", cache->hits);
	printk("  Misses: %u\n", cache->misses);
	printk("  Hit rate: %u%%\n", hit_rate);
}

/**
 * @brief ブロックキャッシュを破棄する
 */
void block_cache_destroy(struct block_cache *cache) {
	if (!cache) {
		return;
	}

	/* dirtyブロックをフラッシュ */
	block_cache_flush(cache);

	/* エントリのデータを解放 */
	for (uint32_t i = 0; i < cache->num_entries; i++) {
		if (cache->entries[i].data) {
			kfree(cache->entries[i].data);
		}
	}

	/* エントリ配列を解放 */
	if (cache->entries) {
		kfree(cache->entries);
	}

	/* キャッシュ本体を解放 */
	kfree(cache);
}