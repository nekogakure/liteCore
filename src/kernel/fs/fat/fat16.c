#include <stdint.h>
#include <stddef.h>
#include <util/console.h>
#include <mem/manager.h>
#include <fs/fat/fat16.h>

extern struct fat16_super *g_fat16_sb;

/* fallback scratch buffer used when kmalloc fails for temporary block buffer */
static uint8_t fat16_scratch[4096];
/* Sector buffer for FAT16 operations (typically 512 bytes) */
static uint8_t fat16_sector_scratch[4096];
/* Dedicated cluster buffer for fat16_read_file/fat16_list operations */
static uint8_t fat16_cluster_scratch[4096];

static int fat16_read_bytes(struct fat16_super *sb, uint32_t offset, void *dst,
			    uint32_t len);
static int fat16_write_bytes(struct fat16_super *sb, uint32_t offset,
			     const void *src, uint32_t len);
static uint16_t fat_read_entry(struct fat16_super *sb, uint16_t cluster);
static void make_shortname(const char *name, char out[11]);

static int fat16_read_sector(struct fat16_super *sb, uint32_t sector,
			     uint8_t *buf) {
	uint32_t off = sector * sb->bytes_per_sector;
	return fat16_read_bytes(sb, off, buf, sb->bytes_per_sector);
}

static int fat16_find_root_entry_bytes(struct fat16_super *sb, const char *name,
				       uint8_t ent_buf[32], uint32_t *ent_off,
				       uint32_t *free_off) {
	uint32_t root_sector = sb->root_dir_sector;
	uint32_t entries_per_sector = sb->bytes_per_sector / 32;
	uint32_t sectors = ((sb->max_root_entries + entries_per_sector - 1) /
			    entries_per_sector);
	/* Use static scratch buffer for sector reads */
	if (sb->bytes_per_sector > sizeof(fat16_sector_scratch)) {
		printk("fat16: bytes_per_sector %u exceeds sector scratch size\n",
		       sb->bytes_per_sector);
		return -2;
	}
	uint8_t *sec = fat16_sector_scratch;
	uint8_t shortname[11];
	make_shortname(name, (char *)shortname);
	/* Debug: log the requested shortname for lookup */
	{
		char dbgname[12];
		for (int i = 0; i < 11; ++i)
			dbgname[i] = shortname[i] ? shortname[i] : ' ';
		dbgname[11] = '\0';
		printk("fat16: find_root_entry looking for '%s' (orig='%s')\n",
		       dbgname, name);
	}
	uint32_t first_free_off = 0xFFFFFFFF;
	for (uint32_t s = 0; s < sectors; ++s) {
		if (fat16_read_sector(sb, root_sector + s, sec) != 0) {
			/* Diagnostic: report which sector failed to read */
			printk("fat16: find_root_entry failed to read sector %u (root_sector=%u, s=%u)\n",
			       root_sector + s, root_sector, s);

			return -2;
		}
		for (uint32_t e = 0; e < entries_per_sector; ++e) {
			//uint32_t ent_base = s * sb->bytes_per_sector + e * 32;
			uint8_t *ent = sec + e * 32;
			if (ent[0] == 0x00) {
				if (free_off)
					*free_off = (root_sector +
						     s) * sb->bytes_per_sector +
						    e * 32;

				return -1; /* end */
			}
			if (ent[0] == 0xE5) {
				if (first_free_off == 0xFFFFFFFF)
					first_free_off =
						(root_sector + s) *
							sb->bytes_per_sector +
						e * 32;
				continue;
			}
			uint8_t attr = ent[11];
			if (attr & 0x08)
				continue;
			int match = 1;
			for (int i = 0; i < 11; ++i) {
				if (ent[i] != (uint8_t)shortname[i]) {
					match = 0;
					break;
				}
			}
			if (match) {
				/* copy entry */
				for (int i = 0; i < 32; ++i)
					ent_buf[i] = ent[i];
				if (ent_off)
					*ent_off = (root_sector +
						    s) * sb->bytes_per_sector +
						   e * 32;
				if (free_off) {
					*free_off =
						(first_free_off == 0xFFFFFFFF) ?
							0xFFFFFFFF :
							first_free_off;
				}

				return 0;
			}
		}
	}
	if (free_off)
		*free_off = (first_free_off == 0xFFFFFFFF) ? 0xFFFFFFFF :
							     first_free_off;

	return -1;
}

static int fat16_find_entry_in_dir_bytes(struct fat16_super *sb,
					 uint16_t start_cluster,
					 const char *name, uint8_t ent_buf[32],
					 uint32_t *ent_off,
					 uint32_t *free_off) {
	uint8_t shortname[11];
	make_shortname(name, (char *)shortname);

	uint32_t entries_per_sector = sb->bytes_per_sector / 32;
	uint32_t first_free_off = 0xFFFFFFFF;
	uint8_t *sec = fat16_sector_scratch;
	uint16_t cur = start_cluster;
	while (cur >= 2 && cur < 0xFFF8) {
		uint32_t sector = sb->first_data_sector +
				  (cur - 2) * sb->sectors_per_cluster;
		for (uint8_t sc = 0; sc < sb->sectors_per_cluster; ++sc) {
			if (fat16_read_sector(sb, sector + sc, sec) != 0) {
				/* Diagnostic: report failing sector / cluster */
				printk("fat16: read sector failed in dir scan sector=%u cluster=%u (sc=%u)\n",
				       sector + sc, cur, sc);

				return -2;
			}
			for (uint32_t e = 0; e < entries_per_sector; ++e) {
				uint8_t *ent = sec + e * 32;
				uint32_t abs_off =
					(sector + sc) * sb->bytes_per_sector +
					e * 32;
				if (ent[0] == 0x00) {
					if (free_off)
						*free_off =
							(first_free_off ==
							 0xFFFFFFFF) ?
								abs_off :
								first_free_off;

					return -1; /* end */
				}
				if (ent[0] == 0xE5) {
					if (first_free_off == 0xFFFFFFFF)
						first_free_off = abs_off;
					continue;
				}
				uint8_t attr = ent[11];
				if (attr & 0x08)
					continue;
				int match = 1;
				for (int i = 0; i < 11; ++i) {
					if (ent[i] != (uint8_t)shortname[i]) {
						match = 0;
						break;
					}
				}
				if (match) {
					for (int i = 0; i < 32; ++i)
						ent_buf[i] = ent[i];
					if (ent_off)
						*ent_off = abs_off;
					if (free_off)
						*free_off =
							(first_free_off ==
							 0xFFFFFFFF) ?
								0xFFFFFFFF :
								first_free_off;

					return 0;
				}
			}
		}
		uint16_t next = fat_read_entry(sb, cur);
		if (next == 0 || next >= 0xFFF8)
			break;
		cur = next;
	}
	if (free_off)
		*free_off = (first_free_off == 0xFFFFFFFF) ? 0xFFFFFFFF :
							     first_free_off;

	return -1;
}

/* 小さな helper 関数群 */
static void mem_copy(void *dst, const void *src, size_t n) {
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	for (size_t i = 0; i < n; ++i)
		d[i] = s[i];
}
static void mem_set(void *dst, int v, size_t n) {
	uint8_t *d = (uint8_t *)dst;
	for (size_t i = 0; i < n; ++i)
		d[i] = (uint8_t)v;
}

static uint16_t le16(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* FAT テーブルの先頭バイトオフセットを返す */
static uint32_t fat_offset_bytes(struct fat16_super *sb) {
	return (uint32_t)sb->reserved_sectors * sb->bytes_per_sector;
}

/* Helper: read arbitrary bytes from either image (in-memory) or block_cache
 * into dst. Returns 0 on success, -1 on failure.
 */
static int fat16_read_bytes(struct fat16_super *sb, uint32_t offset, void *dst,
			    uint32_t len) {
	if (!sb || !dst)
		return -1;
	if (sb->image) {
		/* in-memory image */
		if ((uint64_t)offset + (uint64_t)len > sb->image_size)
			return -1;
		for (uint32_t i = 0; i < len; ++i)
			((uint8_t *)dst)[i] = sb->image[offset + i];
		return 0;
	}
	if (!sb->cache)
		return -1;
	uint32_t bs = sb->cache->block_size;
	/* Always use static scratch buffer to avoid heap fragmentation */
	if (bs > sizeof(fat16_scratch)) {
		printk("fat16: block_size %u exceeds scratch buffer size %u\n",
		       bs, (uint32_t)sizeof(fat16_scratch));
		return -1;
	}
	uint8_t *tmp = fat16_scratch;
	uint32_t start_block = offset / bs;
	uint32_t end_block = (offset + len - 1) / bs;
	uint32_t copied = 0;
	for (uint32_t b = start_block; b <= end_block; ++b) {
		if (block_cache_read(sb->cache, b, tmp) != 0) {
			printk("fat16: block_cache_read failed for block %u (offset=%u len=%u)\n",
			       b, offset, len);
			return -1;
		}
		uint32_t block_off = b * bs;
		uint32_t from = (offset > block_off) ? (offset - block_off) : 0;
		uint32_t avail = bs - from;
		uint32_t need = len - copied;
		uint32_t take = (need < avail) ? need : avail;
		for (uint32_t i = 0; i < take; ++i)
			((uint8_t *)dst)[copied + i] = tmp[from + i];
		copied += take;
	}
	return 0;
}

static int fat16_write_bytes(struct fat16_super *sb, uint32_t offset,
			     const void *src, uint32_t len) {
	if (!sb || !src)
		return -1;
	if (sb->image) {
		if ((uint64_t)offset + (uint64_t)len > sb->image_size)
			return -1;
		for (uint32_t i = 0; i < len; ++i)
			sb->image[offset + i] = ((const uint8_t *)src)[i];
		return 0;
	}
	if (!sb->cache)
		return -1;
	uint32_t bs = sb->cache->block_size;
	/* Always use static scratch buffer to avoid heap fragmentation */
	if (bs > sizeof(fat16_scratch)) {
		printk("fat16: block_size %u exceeds scratch buffer size %u\n",
		       bs, (uint32_t)sizeof(fat16_scratch));
		return -1;
	}
	uint8_t *tmp = fat16_scratch;
	uint32_t start_block = offset / bs;
	uint32_t end_block = (offset + len - 1) / bs;
	uint32_t written = 0;
	for (uint32_t b = start_block; b <= end_block; ++b) {
		if (block_cache_read(sb->cache, b, tmp) != 0) {
			return -1;
		}
		uint32_t block_off = b * bs;
		uint32_t from = (offset > block_off) ? (offset - block_off) : 0;
		uint32_t avail = bs - from;
		uint32_t need = len - written;
		uint32_t take = (need < avail) ? need : avail;
		for (uint32_t i = 0; i < take; ++i)
			tmp[from + i] = ((const uint8_t *)src)[written + i];
		if (block_cache_write(sb->cache, b, tmp) != 0) {
			return -1;
		}
		written += take;
	}
	return 0;
}

/* 指定クラスタの FAT エントリ (16bit) を読み書きする */
static uint16_t fat_read_entry(struct fat16_super *sb, uint16_t cluster) {
	uint32_t off = fat_offset_bytes(sb) + cluster * 2;
	uint8_t buf[2];
	if (fat16_read_bytes(sb, off, buf, 2) != 0)
		return 0xFFFF; /* treat as EOF/error */
	return le16(buf);
}

static void fat_write_entry(struct fat16_super *sb, uint16_t cluster,
			    uint16_t val) {
	uint32_t off = fat_offset_bytes(sb) + cluster * 2;
	uint8_t buf[2];
	buf[0] = (uint8_t)(val & 0xff);
	buf[1] = (uint8_t)((val >> 8) & 0xff);
	/* FAT コピーが複数あるので全て更新する */
	for (uint8_t f = 0; f < sb->num_fats; ++f) {
		uint32_t fat_off = off + (uint32_t)f * sb->fat_size_sectors *
						 sb->bytes_per_sector;
		(void)fat16_write_bytes(sb, fat_off, buf, 2);
	}
}

static int allocate_chain(struct fat16_super *sb, uint16_t n,
			  uint16_t *out_start) {
	if (n == 0)
		return -1;
	uint32_t total_clusters = (sb->total_sectors - sb->first_data_sector) /
				  sb->sectors_per_cluster;
	uint16_t *list = (uint16_t *)kmalloc(sizeof(uint16_t) * n);
	if (!list)
		return -3;
	uint16_t found = 0;
	for (uint16_t c = 2; c < (uint16_t)(2 + total_clusters) && found < n;
	     ++c) {
		uint16_t e = fat_read_entry(sb, c);
		if (e == 0) {
			list[found++] = c;
		}
	}
	if (found < n) {
		kfree(list);
		return -2; /* 空き不足 */
	}
	/* 見つかったクラスタを順にリンクして書き込む */
	for (uint16_t i = 0; i < n; ++i) {
		uint16_t cur = list[i];
		uint16_t val = (i + 1 == n) ? 0xFFFF : list[i + 1];
		fat_write_entry(sb, cur, val);
	}
	*out_start = list[0];
	kfree(list);
	return 0;
}

static void make_shortname(const char *name, char out[11]) {
	/* 空白で埋める */
	for (int i = 0; i < 11; ++i)
		out[i] = ' ';
	int ni = 0;
	int si = 0;
	/* ベース名 8 バイト */
	while (name[si] && name[si] != '.' && ni < 8) {
		char c = name[si++];
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		out[ni++] = c;
	}
	/* 拡張子 */
	if (name[si] == '.')
		si++;
	ni = 8;
	while (name[si] && ni < 11) {
		char c = name[si++];
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		out[ni++] = c;
	}
}

static int fat16_resolve_path_bytes(struct fat16_super *sb, const char *path,
				    uint8_t ent_buf[32], uint32_t *ent_off,
				    uint32_t *free_off,
				    uint16_t *parent_cluster_out) {
	if (!path)
		return -2;
	uint16_t dir_cluster = 0; /* root */
	const char *p = path;
	if (p[0] == '/') {
		while (*p == '/')
			p++;
	}
	char comp[13];
	while (*p) {
		int ci = 0;
		while (*p && *p != '/' && ci < (int)sizeof(comp) - 1)
			comp[ci++] = *p++;
		comp[ci] = '\0';
		while (*p == '/')
			p++;
		int is_last = (*p == '\0');
		/* Debug: component being resolved */
		printk("fat16: resolve component '%s' (is_last=%d) dir_cluster=%u\n",
		       comp, is_last, dir_cluster);
		int r;
		uint32_t found_off = 0;
		uint8_t local_ent[32];
		if (dir_cluster == 0) {
			r = fat16_find_root_entry_bytes(sb, comp, local_ent,
							&found_off, free_off);
		} else {
			r = fat16_find_entry_in_dir_bytes(sb, dir_cluster, comp,
							  local_ent, &found_off,
							  free_off);
		}
		if (r == 0) {
			/* found */
			if (is_last) {
				/* copy out */
				for (int i = 0; i < 32; ++i)
					ent_buf[i] = local_ent[i];
				if (ent_off)
					*ent_off = found_off;
				if (parent_cluster_out)
					*parent_cluster_out = dir_cluster;
				return 0;
			}
			/* intermediate: must be directory */
			uint8_t attr = local_ent[11];
			if (!(attr & 0x10))
				return -2;
			uint16_t next_cluster = le16(local_ent + 26);
			if (next_cluster < 2)
				return -2;
			dir_cluster = next_cluster;
			continue;
		} else if (r == -1) {
			/* not found */
			if (is_last) {
				if (parent_cluster_out)
					*parent_cluster_out = dir_cluster;
				return -1;
			}
			return -2;
		} else {
			return -2;
		}
	}
	return -2;
}

/* マウント */
int fat16_mount(void *image, size_t size, struct fat16_super **out) {
	if (!image || size < 512)
		return -1;
	uint8_t *img = (uint8_t *)image;
	uint16_t bytes_per_sector = le16(img + 11);
	uint8_t sectors_per_cluster = img[13];
	uint16_t reserved = le16(img + 14);
	uint8_t num_fats = img[16];
	uint16_t max_root = le16(img + 17);
	uint16_t total_sectors_short = le16(img + 19);
	uint32_t total_sectors = total_sectors_short ? total_sectors_short :
						       le32(img + 32);
	uint16_t fat_size_sectors = le16(img + 22);
	if (!fat_size_sectors)
		fat_size_sectors = (uint16_t)le32(img + 36);

	if (bytes_per_sector != 512)
		return -2; /* 簡易: 512のみサポート */

	struct fat16_super *sb =
		(struct fat16_super *)kmalloc(sizeof(struct fat16_super));
	if (!sb)
		return -3;
	sb->bytes_per_sector = bytes_per_sector;
	sb->sectors_per_cluster = sectors_per_cluster;
	sb->reserved_sectors = reserved;
	sb->num_fats = num_fats;
	sb->max_root_entries = max_root;
	sb->total_sectors = total_sectors;
	sb->fat_size_sectors = fat_size_sectors;
	sb->image = img;
	sb->image_size = size;

	uint32_t root_dir_sectors =
		((max_root * 32) + (bytes_per_sector - 1)) / bytes_per_sector;
	uint32_t first_data_sector =
		reserved + (num_fats * fat_size_sectors) + root_dir_sectors;
	uint32_t root_dir_sector = reserved + (num_fats * fat_size_sectors);
	sb->first_data_sector = first_data_sector;
	sb->root_dir_sector = root_dir_sector;

	*out = sb;
	return 0;
}

/* ブロックキャッシュ経由で FAT16 をマウントする（キャッシュから全イメージを読み込み、
   メモリ上に展開する簡易実装）。 */
int fat16_mount_with_cache(struct block_cache *cache,
			   struct fat16_super **out) {
	if (!cache || !out)
		return -1;
	uint32_t bs = cache->block_size;
	/* Allocate extra space to avoid canary overwrite by block_cache_read */
	uint8_t *tmp = (uint8_t *)kmalloc(bs + 16);
	if (!tmp)
		return -2;
	/* 読み取り: ブロック0 を読み BPB を解析 */
	if (block_cache_read(cache, 0, tmp) != 0) {
		/* block cache read failed (debug log removed) */
		kfree(tmp);
		return -3;
	}
	uint16_t bytes_per_sector = le16(tmp + 11);
	uint8_t sectors_per_cluster = tmp[13];
	uint16_t reserved = le16(tmp + 14);
	uint8_t num_fats = tmp[16];
	uint16_t max_root = le16(tmp + 17);
	uint16_t total_sectors_short = le16(tmp + 19);
	uint32_t total_sectors = total_sectors_short ? total_sectors_short :
						       le32(tmp + 32);
	uint16_t fat_size_sectors = le16(tmp + 22);

	/* BPB values parsed (debug log removed) */
	if (!fat_size_sectors)
		fat_size_sectors = (uint16_t)le32(tmp + 36);

	/* 簡易チェック */
	if (bytes_per_sector != 512) {
		/* unsupported bytes_per_sector (debug log removed) */
		kfree(tmp);
		return -4;
	}

	/* Do not allocate the entire image; create a superblock that references
	 * the provided block cache and parses BPB values. All subsequent
	 * filesystem operations will use the cache to read/write sectors.
	 */
	struct fat16_super *sb =
		(struct fat16_super *)kmalloc(sizeof(struct fat16_super));
	if (!sb) {
		kfree(tmp);
		return -5;
	}
	sb->bytes_per_sector = bytes_per_sector;
	sb->sectors_per_cluster = sectors_per_cluster;
	sb->reserved_sectors = reserved;
	sb->num_fats = num_fats;
	sb->max_root_entries = max_root;
	sb->total_sectors = total_sectors;
	sb->fat_size_sectors = fat_size_sectors;
	sb->cache = cache;
	sb->image = NULL;
	sb->image_size = 0;

	uint32_t root_dir_sectors =
		((max_root * 32) + (bytes_per_sector - 1)) / bytes_per_sector;
	uint32_t first_data_sector =
		reserved + (num_fats * fat_size_sectors) + root_dir_sectors;
	uint32_t root_dir_sector = reserved + (num_fats * fat_size_sectors);
	sb->first_data_sector = first_data_sector;
	sb->root_dir_sector = root_dir_sector;

	kfree(tmp);
	*out = sb;
	/* expose mounted fat16 superblock to global for legacy init checks */
	g_fat16_sb = sb;
	return 0;
}

/* ルート一覧 */
int fat16_list_root(struct fat16_super *sb) {
	if (!sb)
		return -1;
	uint32_t root_sector = sb->root_dir_sector;
	uint32_t entries_per_sector = sb->bytes_per_sector / 32;
	uint32_t sectors = ((sb->max_root_entries + entries_per_sector - 1) /
			    entries_per_sector);
	char name[13];
	/* Use static scratch buffer for sector reads */
	uint8_t *sec = fat16_sector_scratch;
	for (uint32_t s = 0; s < sectors; ++s) {
		if (fat16_read_sector(sb, root_sector + s, sec) != 0) {
			return -1;
		}
		for (uint32_t e = 0; e < entries_per_sector; ++e) {
			uint8_t *ent = sec + e * 32;
			if (ent[0] == 0x00) {
				return 0;
			}
			if (ent[0] == 0xE5)
				continue;
			uint8_t attr = ent[11];
			if (attr & 0x08)
				continue;
			mem_set(name, 0, sizeof(name));
			int ni = 0;
			for (int i = 0; i < 8; ++i) {
				char c = ent[i];
				if (c == ' ')
					break;
				name[ni++] = c;
			}
			if (ent[8] != ' ') {
				name[ni++] = '.';
				for (int i = 0; i < 3; ++i) {
					char c = ent[8 + i];
					if (c == ' ')
						break;
					name[ni++] = c;
				}
			}
			uint32_t file_size = le32(ent + 28);
			/* skip '.' and '..' entries */
			if (name[0] == '.' &&
			    (name[1] == '\0' ||
			     (name[1] == '.' && name[2] == '\0')))
				continue;

			/* align name to fixed column for consistent VGA/serial output */
			int namelen = 0;
			while (name[namelen])
				namelen++;
			int pad = 16 - namelen;
			if (pad < 1)
				pad = 1;
			printk("%s", name);
			for (int _i = 0; _i < pad; ++_i)
				printk(" ");
			printk("[FILE] %u bytes\n", file_size);
		}
	}

	return 0;
}

/* List a directory specified by absolute path. If path is "/" lists root.
 * Output format (as requested):
 *   file: "name\t[FILE] XXX bytes"
 *   dir:  "name\t[DIR ] XXX bytes"
 */
int fat16_list_dir(struct fat16_super *sb, const char *path) {
	if (!sb || !path)
		return -1;
	/* root path */
	if (path[0] == '/' && (path[1] == '\0' || path[1] == '/'))
		return fat16_list_root(sb);

	/* resolve path to directory entry */
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	uint16_t parent = 0;
	int r = fat16_resolve_path_bytes(sb, path, ent_buf, &ent_off, &free_off,
					 &parent);
	if (r != 0)
		return -1;
	uint8_t attr = ent_buf[11];
	if (!(attr & 0x10))
		return -1; /* not a directory */

	uint16_t start_cluster = le16(ent_buf + 26);
	/* if start_cluster == 0, it's root */
	if (start_cluster == 0)
		return fat16_list_root(sb);

	uint32_t entries_per_sector = sb->bytes_per_sector / 32;
	/* Use static scratch buffer for sector reads */
	uint8_t *sec = fat16_sector_scratch;

	uint16_t cur = start_cluster;
	while (cur >= 2 && cur < 0xFFF8) {
		uint32_t sector = sb->first_data_sector +
				  (cur - 2) * sb->sectors_per_cluster;
		for (uint8_t sc = 0; sc < sb->sectors_per_cluster; ++sc) {
			if (fat16_read_sector(sb, sector + sc, sec) != 0) {
				return -1;
			}
			for (uint32_t e = 0; e < entries_per_sector; ++e) {
				uint8_t *ent = sec + e * 32;
				if (ent[0] == 0x00) {
					return 0; /* end of dir */
				}
				if (ent[0] == 0xE5)
					continue;
				uint8_t eattr = ent[11];
				if (eattr & 0x08)
					continue;
				char name[13];
				mem_set(name, 0, sizeof(name));
				int ni = 0;
				for (int i = 0; i < 8; ++i) {
					char c = ent[i];
					if (c == ' ')
						break;
					name[ni++] = c;
				}
				if (ent[8] != ' ') {
					name[ni++] = '.';
					for (int i = 0; i < 3; ++i) {
						char c = ent[8 + i];
						if (c == ' ')
							break;
						name[ni++] = c;
					}
				}
				uint32_t file_size = le32(ent + 28);
				/* skip '.' and '..' entries */
				if (name[0] == '.' &&
				    (name[1] == '\0' ||
				     (name[1] == '.' && name[2] == '\0')))
					continue;
				/* align name to fixed column for consistent VGA/serial output */
				int namelen = 0;
				while (name[namelen])
					namelen++;
				int pad = 16 - namelen;
				if (pad < 1)
					pad = 1;
				printk("%s", name);
				for (int _i = 0; _i < pad; ++_i)
					printk(" ");
				if (eattr & 0x10) {
					/* directory */
					printk("[DIR ] %u bytes\n", file_size);
				} else {
					printk("[FILE] %u bytes\n", file_size);
				}
			}
		}
		uint16_t next = fat_read_entry(sb, cur);
		if (next == 0 || next >= 0xFFF8)
			break;
		cur = next;
	}

	return 0;
}

int fat16_get_file_size(struct fat16_super *sb, const char *name,
			uint32_t *out_size) {
	if (!sb || !name || !out_size)
		return -1;
	printk("fat16: fat16_get_file_size called for '%s'\n", name);
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	uint16_t parent = 0;
	int r = fat16_resolve_path_bytes(sb, name, ent_buf, &ent_off, &free_off,
					 &parent);
	printk("fat16: fat16_get_file_size -> resolve r=%d\n", r);
	if (r != 0)
		return -2;
	uint32_t size = le32(ent_buf + 28);
	*out_size = size;
	return 0;
}

int fat16_is_dir(struct fat16_super *sb, const char *path) {
	if (!sb || !path)
		return 0;
	if (path[0] == '/' && (path[1] == '\0' || path[1] == '/'))
		return 1; /* root */
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	uint16_t parent = 0;
	int r = fat16_resolve_path_bytes(sb, path, ent_buf, &ent_off, &free_off,
					 &parent);
	if (r != 0)
		return 0;
	uint8_t attr = ent_buf[11];
	return (attr & 0x10) ? 1 : 0;
}

/* ファイルを読み出す (最初のクラスタのみを想定) */
int fat16_read_file(struct fat16_super *sb, const char *name, void *buf,
		    size_t len, size_t *out_len) {
	if (!sb || !name)
		return -1;
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	uint16_t parent_cluster = 0;
	int rr = fat16_resolve_path_bytes(sb, name, ent_buf, &ent_off,
					  &free_off, &parent_cluster);
	if (rr != 0)
		return -2;
	uint16_t start_cluster = le16(ent_buf + 26);
	uint32_t file_size = le32(ent_buf + 28);
	printk("fat16: read_file name='%s' start_cluster=%u file_size=%u len=%u\n",
	       name, start_cluster, file_size, (uint32_t)len);
	if (file_size == 0) {
		if (out_len)
			*out_len = 0;
		return 0; /* 空ファイル */
	}
	if (start_cluster < 2)
		return -3;
	uint32_t bytes_to_read = file_size;
	if (bytes_to_read > len)
		bytes_to_read = len;
	uint32_t cluster_bytes = sb->bytes_per_sector * sb->sectors_per_cluster;
	uint32_t bytes_read = 0;
	uint16_t cur = start_cluster;

	if (cluster_bytes > sizeof(fat16_cluster_scratch)) {
		printk("fat16: cluster_bytes %u exceeds cluster_scratch size %u\n",
		       cluster_bytes, (uint32_t)sizeof(fat16_cluster_scratch));
		return -1;
	}

	uint8_t *cluster_buf = fat16_cluster_scratch;
	while (cur >= 2 && cur < 0xFFF8 && bytes_read < bytes_to_read) {
		uint32_t sector = sb->first_data_sector +
				  (cur - 2) * sb->sectors_per_cluster;
		/* read cluster (multiple sectors) */
		for (uint8_t sc = 0; sc < sb->sectors_per_cluster; ++sc) {
			if (fat16_read_sector(
				    sb, sector + sc,
				    cluster_buf + sc * sb->bytes_per_sector) !=
			    0) {
				printk("fat16: read_sector failed for sector %u (cluster %u)\n",
				       sector + sc, cur);
				return -1;
			}
		}
		uint32_t copy = bytes_to_read - bytes_read;
		if (copy > cluster_bytes)
			copy = cluster_bytes;
		mem_copy((uint8_t *)buf + bytes_read, cluster_buf, copy);
		bytes_read += copy;
		uint16_t next = fat_read_entry(sb, cur);
		if (next == 0 || next >= 0xFFF8)
			break;
		cur = next;
	}
	if (out_len)
		*out_len = bytes_read;
	return 0;
}

/* 連続割当の古い実装は削除し、allocate_chain を使用します */

/* 既存チェーンを解放 (FAT エントリを 0 に戻す) */
static void free_chain(struct fat16_super *sb, uint16_t start) {
	uint16_t cur = start;
	while (cur >= 2 && cur != 0 && cur < 0xFFF8) {
		uint16_t next = fat_read_entry(sb, cur);
		fat_write_entry(sb, cur, 0);
		if (next == 0 || next >= 0xFFF8)
			break;
		cur = next;
	}
}

/* ファイル作成: 空ファイル */
int fat16_create_file(struct fat16_super *sb, const char *name) {
	if (!sb || !name)
		return -1;
	//uint8_t *existing = NULL;
	//uint8_t *free_slot = NULL;
	uint16_t parent_cluster = 0;
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	int r = fat16_resolve_path_bytes(sb, name, ent_buf, &ent_off, &free_off,
					 &parent_cluster);
	if (r == 0) {
		/* existing: clear clusters and zero size/start in directory entry */
		uint16_t start = le16(ent_buf + 26);
		if (start >= 2)
			free_chain(sb, start);
		/* zero out entry in-place */
		uint8_t zero32[32];
		for (int i = 0; i < 32; ++i)
			zero32[i] = 0;
		(void)fat16_write_bytes(sb, ent_off, zero32, 32);
		return 0;
	} else if (r == -1) {
		/* not found: free_off holds offset for free slot */
		if (free_off == 0xFFFFFFFF)
			return -2; /* no free slot */
		/* create entry bytes and write them */
		char shortname[11];
		make_shortname(name, shortname);
		uint8_t newent[32];
		for (int i = 0; i < 32; ++i)
			newent[i] = 0;
		for (int i = 0; i < 11; ++i)
			newent[i] = (uint8_t)shortname[i];
		newent[11] = 0x20; /* archive */
		/* start cluster and size left zero */
		(void)fat16_write_bytes(sb, free_off, newent, 32);
		return 0;
	}
	return -2;
}

/* ファイル書き込み: 上書き (既存は解放、連続クラスタを割当して書く) */
int fat16_write_file(struct fat16_super *sb, const char *name, const void *buf,
		     size_t len) {
	if (!sb || !name || !buf)
		return -1;
	//uint8_t *free_slot = NULL;
	uint16_t parent_cluster = 0;
	uint8_t ent_buf[32];
	uint32_t ent_off = 0;
	uint32_t free_off = 0;
	int r = fat16_resolve_path_bytes(sb, name, ent_buf, &ent_off, &free_off,
					 &parent_cluster);
	uint8_t ent_local[32];
	if (r == 0) {
		/* existing */
		uint16_t old_start = le16(ent_buf + 26);
		if (old_start >= 2)
			free_chain(sb, old_start);
		/* we'll overwrite entry after writing data */
	} else if (r == -1) {
		if (free_off == 0xFFFFFFFF)
			return -2;
		/* prepare new entry template in ent_local */
		char shortname[11];
		make_shortname(name, shortname);
		for (int i = 0; i < 32; ++i)
			ent_local[i] = 0;
		for (int i = 0; i < 11; ++i)
			ent_local[i] = (uint8_t)shortname[i];
		ent_local[11] = 0x20;
	} else {
		return -2;
	}
	/* 必要クラスタ数を計算 */
	uint32_t cluster_bytes = sb->bytes_per_sector * sb->sectors_per_cluster;
	uint16_t need = (uint16_t)((len + cluster_bytes - 1) / cluster_bytes);
	/* len == 0 の場合はクラスタ割当を行わずエントリを空にする */
	if (len == 0) {
		uint32_t entry_offset = (r == 0) ? ent_off : free_off;
		if (entry_offset == 0xFFFFFFFF)
			return -1;
		uint8_t update_ent[32];
		if (r == 0) {
			for (int i = 0; i < 32; ++i)
				update_ent[i] = ent_buf[i];
		} else {
			for (int i = 0; i < 32; ++i)
				update_ent[i] = ent_local[i];
		}

		update_ent[26] = 0;
		update_ent[27] = 0;
		update_ent[28] = update_ent[29] = update_ent[30] =
			update_ent[31] = 0;
		(void)fat16_write_bytes(sb, entry_offset, update_ent, 32);
		return 0;
	}
	if (need == 0)
		need = 1;
	uint16_t start_cluster;
	/* 非連続クラスタでも割当可能な allocate_chain を使う */
	if (allocate_chain(sb, need, &start_cluster) != 0)
		return -3;
	/* データ領域へ書き込み (チェーンに従い順次書き込む) */
	uint32_t remaining = (uint32_t)len;
	uint16_t cur = start_cluster;
	uint32_t written = 0;
	/* Always use dedicated cluster scratch buffer to avoid conflicts */
	if (cluster_bytes > sizeof(fat16_cluster_scratch)) {
		printk("fat16: cluster_bytes %u exceeds cluster_scratch size %u\n",
		       cluster_bytes, (uint32_t)sizeof(fat16_cluster_scratch));
		return -1;
	}
	uint8_t *cluster_buf = fat16_cluster_scratch;
	while (cur >= 2 && cur < 0xFFF8 && remaining > 0) {
		uint32_t sector = sb->first_data_sector +
				  (cur - 2) * sb->sectors_per_cluster;
		uint32_t to_copy = remaining > cluster_bytes ? cluster_bytes :
							       remaining;
		/* fill cluster_buf from source data */
		for (uint32_t i = 0; i < to_copy; ++i)
			cluster_buf[i] = ((const uint8_t *)buf)[written + i];
		/* zero the rest if any */
		for (uint32_t i = to_copy; i < cluster_bytes; ++i)
			cluster_buf[i] = 0;
		/* write out cluster sector-by-sector */
		for (uint8_t sc = 0; sc < sb->sectors_per_cluster; ++sc) {
			uint32_t off = (sector + sc) * sb->bytes_per_sector;
			if (fat16_write_bytes(sb, off,
					      cluster_buf +
						      sc * sb->bytes_per_sector,
					      sb->bytes_per_sector) != 0) {
				return -1;
			}
		}
		written += to_copy;
		remaining -= to_copy;
		uint16_t next = fat_read_entry(sb, cur);
		if (next == 0 || next >= 0xFFF8)
			break;
		cur = next;
	}
	/* ディレクトリエントリの start cluster と size を更新 */
	uint32_t entry_offset = (r == 0) ? ent_off : free_off;
	if (entry_offset == 0xFFFFFFFF)
		return -1;
	uint8_t update_ent[32];
	if (r == 0) {
		for (int i = 0; i < 32; ++i)
			update_ent[i] = ent_buf[i];
	} else {
		for (int i = 0; i < 32; ++i)
			update_ent[i] = ent_local[i];
	}
	update_ent[26] = (uint8_t)(start_cluster & 0xff);
	update_ent[27] = (uint8_t)((start_cluster >> 8) & 0xff);
	uint32_t size = (uint32_t)len;
	update_ent[28] = (uint8_t)(size & 0xff);
	update_ent[29] = (uint8_t)((size >> 8) & 0xff);
	update_ent[30] = (uint8_t)((size >> 16) & 0xff);
	update_ent[31] = (uint8_t)((size >> 24) & 0xff);
	(void)fat16_write_bytes(sb, entry_offset, update_ent, 32);
	return 0;
}
