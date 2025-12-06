#include <stdint.h>
#include <stddef.h>
#include <util/console.h>
#include <mem/manager.h>
#include <fs/ext/ext2.h>
#include <fs/block_cache.h>

/* ヘルパー関数 */
static void mem_copy(void *dst, const void *src, size_t n) {
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
}

static void mem_set(void *dst, int v, size_t n) {
	uint8_t *d = (uint8_t *)dst;
	for (size_t i = 0; i < n; i++)
		d[i] = (uint8_t)v;
}
/*
static int str_cmp(const char *a, const char *b) {
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return (*a == *b) ? 0 : (*a - *b);
}
*/
static size_t str_len(const char *s) {
	size_t len = 0;
	while (s[len])
		len++;
	return len;
}

/* リトルエンディアン読み取り */
static uint16_t le16(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void write_le16(uint8_t *p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_le32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

/**
 * @brief ext2イメージをマウントする
 */
int ext2_mount(const void *image, size_t size, struct ext2_super **out) {
	if (!image || size < 2048 || !out)
		return -1;

	const uint8_t *img = (const uint8_t *)image;

	/* スーパーブロックは1024バイトオフセットから */
	const uint8_t *sb_data = img + 1024;

	/* マジックナンバーチェック (オフセット56) */
	uint16_t magic = le16(sb_data + 56);
	if (magic != EXT2_SUPER_MAGIC) {
		return -2; /* ext2ではない */
	}

	/* ext2_superを確保 */
	struct ext2_super *sb =
		(struct ext2_super *)kmalloc(sizeof(struct ext2_super));
	if (!sb)
		return -3;

	mem_set(sb, 0, sizeof(struct ext2_super));
	sb->image = img;
	sb->image_size = size;

	/* スーパーブロックのフィールドを読み取る */
	sb->sb.s_inodes_count = le32(sb_data + 0);
	sb->sb.s_blocks_count = le32(sb_data + 4);
	sb->sb.s_r_blocks_count = le32(sb_data + 8);
	sb->sb.s_free_blocks_count = le32(sb_data + 12);
	sb->sb.s_free_inodes_count = le32(sb_data + 16);
	sb->sb.s_first_data_block = le32(sb_data + 20);
	sb->sb.s_log_block_size = le32(sb_data + 24);
	sb->sb.s_log_frag_size = le32(sb_data + 28);
	sb->sb.s_blocks_per_group = le32(sb_data + 32);
	sb->sb.s_frags_per_group = le32(sb_data + 36);
	sb->sb.s_inodes_per_group = le32(sb_data + 40);
	sb->sb.s_mtime = le32(sb_data + 44);
	sb->sb.s_wtime = le32(sb_data + 48);
	sb->sb.s_mnt_count = le16(sb_data + 52);
	sb->sb.s_max_mnt_count = le16(sb_data + 54);
	sb->sb.s_magic = le16(sb_data + 56);
	sb->sb.s_state = le16(sb_data + 58);
	sb->sb.s_errors = le16(sb_data + 60);
	sb->sb.s_minor_rev_level = le16(sb_data + 62);
	sb->sb.s_lastcheck = le32(sb_data + 64);
	sb->sb.s_checkinterval = le32(sb_data + 68);
	sb->sb.s_creator_os = le32(sb_data + 72);
	sb->sb.s_rev_level = le32(sb_data + 76);
	sb->sb.s_def_resuid = le16(sb_data + 80);
	sb->sb.s_def_resgid = le16(sb_data + 82);

	/* 拡張フィールド (rev_level >= 1) */
	if (sb->sb.s_rev_level >= 1) {
		sb->sb.s_first_ino = le32(sb_data + 84);
		sb->sb.s_inode_size = le16(sb_data + 88);
	} else {
		sb->sb.s_first_ino = 11;
		sb->sb.s_inode_size = 128;
	}

	/* ブロックサイズを計算 */
	sb->block_size = 1024 << sb->sb.s_log_block_size;

	/* ブロックグループ数を計算 */
	sb->num_groups =
		(sb->sb.s_blocks_count + sb->sb.s_blocks_per_group - 1) /
		sb->sb.s_blocks_per_group;

	/* グループディスクリプタテーブルの位置 */
	/* スーパーブロックの次のブロックから始まる */
	uint32_t sb_block = (sb->sb.s_first_data_block == 0) ? 1 : 2;
	/* group_desc_offset はブロック番号として保持する（下流の処理はブロック番号を期待している） */
	sb->group_desc_offset = sb_block;

	*out = sb;
	return 0;
}

/**
 * @brief inode番号からinodeを読み取る
 */
int ext2_read_inode(struct ext2_super *sb, uint32_t inode_num,
		    struct ext2_inode *inode) {
	if (!sb || !inode || inode_num == 0)
		return -1;

	/* inode番号は1から始まる */
	uint32_t inode_index = inode_num - 1;

	/* どのブロックグループに属するか */
	uint32_t group = inode_index / sb->sb.s_inodes_per_group;
	uint32_t local_index = inode_index % sb->sb.s_inodes_per_group;

	if (group >= sb->num_groups)
		return -2;

	/* グループディスクリプタを読み取る */
	uint8_t gd_block[4096];
	uint32_t gd_block_num =
		sb->group_desc_offset + (group * 32) / sb->block_size;
	uint32_t gd_offset_in_block = (group * 32) % sb->block_size;

	if (sb->cache) {
		/* キャッシュ経由で読み込み */
		if (block_cache_read(sb->cache, gd_block_num, gd_block) != 0)
			return -3;
	} else {
		/* レガシー：直接イメージから読み込み */
		const uint8_t *gd_data =
			sb->image + gd_block_num * sb->block_size;
		mem_copy(gd_block, gd_data, sb->block_size);
	}

	const uint8_t *gd_data = gd_block + gd_offset_in_block;
	uint32_t inode_table = le32(gd_data + 8);

	/* inodeテーブル内のオフセット */
	uint32_t inode_size = (sb->sb.s_inode_size > 0) ? sb->sb.s_inode_size :
							  128;
	uint32_t inode_block_num =
		inode_table + (local_index * inode_size) / sb->block_size;
	uint32_t inode_offset_in_block =
		(local_index * inode_size) % sb->block_size;

	/* inodeデータを読み込む */
	uint8_t inode_block[4096];
	if (sb->cache) {
		if (block_cache_read(sb->cache, inode_block_num, inode_block) !=
		    0)
			return -4;
	} else {
		/* レガシー */
		if (inode_block_num * sb->block_size + inode_size >
		    sb->image_size)
			return -5;
		const uint8_t *inode_data_src =
			sb->image + inode_block_num * sb->block_size;
		mem_copy(inode_block, inode_data_src, sb->block_size);
	}

	const uint8_t *inode_data = inode_block + inode_offset_in_block;

	/* inodeフィールドを読み取る */
	mem_set(inode, 0, sizeof(struct ext2_inode));
	inode->i_mode = le16(inode_data + 0);
	inode->i_uid = le16(inode_data + 2);
	inode->i_size = le32(inode_data + 4);
	inode->i_atime = le32(inode_data + 8);
	inode->i_ctime = le32(inode_data + 12);
	inode->i_mtime = le32(inode_data + 16);
	inode->i_dtime = le32(inode_data + 20);
	inode->i_gid = le16(inode_data + 24);
	inode->i_links_count = le16(inode_data + 26);
	inode->i_blocks = le32(inode_data + 28);
	inode->i_flags = le32(inode_data + 32);
	inode->i_osd1 = le32(inode_data + 36);

	/* ブロックポインタ (15個) */
	for (int i = 0; i < 15; i++) {
		inode->i_block[i] = le32(inode_data + 40 + i * 4);
	}

	inode->i_generation = le32(inode_data + 100);
	inode->i_file_acl = le32(inode_data + 104);
	inode->i_dir_acl = le32(inode_data + 108);
	inode->i_faddr = le32(inode_data + 112);

	return 0;
}

/**
 * @brief ディレクトリinodeからファイル名を検索する
 */
int ext2_find_file_in_dir(struct ext2_super *sb, struct ext2_inode *dir_inode,
			  const char *name, uint32_t *out_inode) {
	if (!sb || !dir_inode || !name || !out_inode)
		return -1;

	/* ディレクトリでない場合はエラー */
	if ((dir_inode->i_mode & 0xF000) != EXT2_S_IFDIR)
		return -2;

	uint32_t dir_size = dir_inode->i_size;
	if (dir_size == 0)
		return -3;

	/* 簡易実装: 直接ブロックポインタのみ対応 (最初の12ブロック) */
	uint32_t read_offset = 0;

	for (int block_idx = 0; block_idx < 12 && read_offset < dir_size &&
				dir_inode->i_block[block_idx] != 0;
	     block_idx++) {
		uint32_t block_num = dir_inode->i_block[block_idx];

		/* ブロックデータを読み込む */
		uint8_t block_data[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, block_num,
					     block_data) != 0)
				return -4;
		} else {
			/* レガシー */
			uint32_t block_offset = block_num * sb->block_size;
			if (block_offset + sb->block_size > sb->image_size)
				return -4;
			mem_copy(block_data, sb->image + block_offset,
				 sb->block_size);
		}

		uint32_t offset = 0;

		/* ディレクトリエントリを順次読み取る */
		while (offset < sb->block_size && read_offset < dir_size) {
			const uint8_t *entry = block_data + offset;
			uint32_t inode = le32(entry + 0);
			uint16_t rec_len = le16(entry + 4);
			uint8_t name_len = entry[6];

			if (rec_len == 0)
				break;

			if (inode != 0 && name_len > 0) {
				/* ファイル名比較 */
				const char *entry_name =
					(const char *)(entry + 8);
				if (name_len == str_len(name)) {
					int match = 1;
					for (uint8_t i = 0; i < name_len; i++) {
						if (entry_name[i] != name[i]) {
							match = 0;
							break;
						}
					}
					if (match) {
						*out_inode = inode;
						return 0;
					}
				}
			}

			offset += rec_len;
			read_offset += rec_len;
		}
	}

	return -5; /* ファイルが見つからない */
}

/**
 * @brief ルートディレクトリの内容を表示する
 */
int ext2_list_root(struct ext2_super *sb) {
	if (!sb)
		return -1;

	struct ext2_inode root_inode;
	int r = ext2_read_inode(sb, EXT2_ROOT_INO, &root_inode);
	if (r != 0) {
		printk("ext2_list_root: Failed to read root inode\n");
		return r;
	}

	uint32_t dir_size = root_inode.i_size;
	uint32_t read_offset = 0;
	int entry_count = 0;

	for (int block_idx = 0; block_idx < 12 && read_offset < dir_size &&
				root_inode.i_block[block_idx] != 0;
	     block_idx++) {
		uint32_t block_num = root_inode.i_block[block_idx];

		uint8_t block_data[4096]; /* 最大ブロックサイズ */
		if (sb->cache) {
			if (block_cache_read(sb->cache, block_num,
					     block_data) != 0)
				break;
		} else {
			/* レガシー */
			uint32_t block_offset = block_num * sb->block_size;
			if (block_offset + sb->block_size > sb->image_size)
				break;
			mem_copy(block_data, sb->image + block_offset,
				 sb->block_size);
		}

		uint32_t offset = 0;

		while (offset < sb->block_size && read_offset < dir_size) {
			const uint8_t *entry = block_data + offset;
			uint32_t inode = le32(entry + 0);
			uint16_t rec_len = le16(entry + 4);
			uint8_t name_len = entry[6];
			uint8_t file_type = entry[7];

			if (rec_len == 0)
				break;

			if (inode != 0 && name_len > 0) {
				char name[256];
				mem_set(name, 0, sizeof(name));
				for (uint8_t i = 0; i < name_len && i < 255;
				     i++) {
					name[i] = entry[8 + i];
				}

				const char *type_str = "UNKNOWN";
				if (file_type == EXT2_FT_REG_FILE)
					type_str = "FILE";
				else if (file_type == EXT2_FT_DIR)
					type_str = "DIR";
				else if (file_type == EXT2_FT_SYMLINK)
					type_str = "SYMLINK";

				printk("  %-20s [%-4s]\n", name, type_str,
				       inode);
				entry_count++;
			}

			offset += rec_len;
			read_offset += rec_len;
		}
	}

	return entry_count;
}

/**
 * @brief ファイル名からファイルを読み出す（ルートディレクトリのみ対応）
 */
int ext2_read_file(struct ext2_super *sb, const char *name, void *buf,
		   size_t len, size_t *out_len) {
	if (!sb || !name || !buf)
		return -1;

	/* ルートinodeを読み取る */
	struct ext2_inode root_inode;
	int r = ext2_read_inode(sb, EXT2_ROOT_INO, &root_inode);
	if (r != 0)
		return r;

	/* ファイル名を検索 */
	uint32_t file_inode_num;
	r = ext2_find_file_in_dir(sb, &root_inode, name, &file_inode_num);
	if (r != 0)
		return r;

	/* ファイルinodeを読み取る */
	struct ext2_inode file_inode;
	r = ext2_read_inode(sb, file_inode_num, &file_inode);
	if (r != 0)
		return r;

	/* 通常ファイルでない場合はエラー */
	if ((file_inode.i_mode & 0xF000) != EXT2_S_IFREG)
		return -2;

	uint32_t file_size = file_inode.i_size;
	uint32_t bytes_to_read = (file_size < len) ? file_size : len;
	uint32_t bytes_read = 0;

	/* 簡易実装: 直接ブロックポインタのみ対応 (最初の12ブロック) */
	for (int block_idx = 0; block_idx < 12 && bytes_read < bytes_to_read &&
				file_inode.i_block[block_idx] != 0;
	     block_idx++) {
		uint32_t block_num = file_inode.i_block[block_idx];

		/* ブロックデータを読み込む */
		uint8_t block_data[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, block_num,
					     block_data) != 0)
				break;
		} else {
			uint32_t block_offset = block_num * sb->block_size;
			if (block_offset + sb->block_size > sb->image_size)
				break;
			mem_copy(block_data, sb->image + block_offset,
				 sb->block_size);
		}

		uint32_t copy_size = sb->block_size;
		if (bytes_read + copy_size > bytes_to_read)
			copy_size = bytes_to_read - bytes_read;

		mem_copy((uint8_t *)buf + bytes_read, block_data, copy_size);
		bytes_read += copy_size;
	}

	if (out_len)
		*out_len = bytes_read;

	return 0;
}

/**
 * @brief inodeのブロックを読み取る（間接ブロック対応）
 * 
 * ext2のブロックポインタ構造:
 * - i_block[0-11]: 直接ブロックポインタ（12個）
 * - i_block[12]: 間接ブロックポインタ（1段）
 * - i_block[13]: 二重間接ブロックポインタ（2段）
 * - i_block[14]: 三重間接ブロックポインタ（3段）
 */
int ext2_get_block_num(struct ext2_super *sb, struct ext2_inode *inode,
		       uint32_t block_index, uint32_t *out_block_num) {
	if (!sb || !inode || !out_block_num)
		return -1;

	uint32_t ptrs_per_block =
		sb->block_size / 4; /* 1ブロックあたりのポインタ数 */

	/* 直接ブロックポインタ (0-11) */
	if (block_index < 12) {
		*out_block_num = inode->i_block[block_index];
		return 0;
	}
	block_index -= 12;

	/* 間接ブロックポインタ (12) */
	if (block_index < ptrs_per_block) {
		uint32_t indirect_block = inode->i_block[12];
		if (indirect_block == 0)
			return -2;

		/* 間接ブロックを読み込む */
		uint8_t indirect_data[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, indirect_block,
					     indirect_data) != 0)
				return -3;
		} else {
			uint32_t offset = indirect_block * sb->block_size;
			if (offset + sb->block_size > sb->image_size)
				return -3;
			mem_copy(indirect_data, sb->image + offset,
				 sb->block_size);
		}

		*out_block_num = le32(indirect_data + block_index * 4);
		return 0;
	}
	block_index -= ptrs_per_block;

	/* 二重間接ブロックポインタ (13) */
	if (block_index < ptrs_per_block * ptrs_per_block) {
		uint32_t double_indirect = inode->i_block[13];
		if (double_indirect == 0)
			return -2;

		/* 第1段のインデックス */
		uint32_t idx1 = block_index / ptrs_per_block;
		uint32_t idx2 = block_index % ptrs_per_block;

		/* 第1段のブロックを読み込む */
		uint8_t block1[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, double_indirect,
					     block1) != 0)
				return -3;
		} else {
			uint32_t offset1 = double_indirect * sb->block_size;
			if (offset1 + sb->block_size > sb->image_size)
				return -3;
			mem_copy(block1, sb->image + offset1, sb->block_size);
		}

		uint32_t indirect_block = le32(block1 + idx1 * 4);
		if (indirect_block == 0)
			return -2;

		/* 第2段のブロックを読み込む */
		uint8_t block2[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, indirect_block,
					     block2) != 0)
				return -3;
		} else {
			uint32_t offset2 = indirect_block * sb->block_size;
			if (offset2 + sb->block_size > sb->image_size)
				return -3;
			mem_copy(block2, sb->image + offset2, sb->block_size);
		}

		*out_block_num = le32(block2 + idx2 * 4);
		return 0;
	}
	block_index -= ptrs_per_block * ptrs_per_block;

	/* 三重間接ブロックポインタ (14) */
	if (block_index < ptrs_per_block * ptrs_per_block * ptrs_per_block) {
		uint32_t triple_indirect = inode->i_block[14];
		if (triple_indirect == 0)
			return -2;

		/* 各段のインデックス */
		uint32_t idx1 = block_index / (ptrs_per_block * ptrs_per_block);
		uint32_t idx2 = (block_index / ptrs_per_block) % ptrs_per_block;
		uint32_t idx3 = block_index % ptrs_per_block;

		/* 第1段 */
		uint8_t block1[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, triple_indirect,
					     block1) != 0)
				return -3;
		} else {
			uint32_t offset1 = triple_indirect * sb->block_size;
			if (offset1 + sb->block_size > sb->image_size)
				return -3;
			mem_copy(block1, sb->image + offset1, sb->block_size);
		}

		uint32_t double_indirect = le32(block1 + idx1 * 4);
		if (double_indirect == 0)
			return -2;

		/* 第2段 */
		uint8_t block2[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, double_indirect,
					     block2) != 0)
				return -3;
		} else {
			uint32_t offset2 = double_indirect * sb->block_size;
			if (offset2 + sb->block_size > sb->image_size)
				return -3;
			mem_copy(block2, sb->image + offset2, sb->block_size);
		}

		uint32_t indirect_block = le32(block2 + idx2 * 4);
		if (indirect_block == 0)
			return -2;

		/* 第3段 */
		uint8_t block3[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, indirect_block,
					     block3) != 0)
				return -3;
		} else {
			uint32_t offset3 = indirect_block * sb->block_size;
			if (offset3 + sb->block_size > sb->image_size)
				return -3;
			mem_copy(block3, sb->image + offset3, sb->block_size);
		}

		*out_block_num = le32(block3 + idx3 * 4);
		return 0;
	}

	return -4; /* ブロックインデックスが範囲外 */
}

/**
 * @brief inodeのデータを読み取る（完全版、間接ブロック対応）
 */
int ext2_read_inode_data(struct ext2_super *sb, struct ext2_inode *inode,
			 void *buf, size_t len, uint32_t offset,
			 size_t *out_len) {
	if (!sb || !inode || !buf)
		return -1;

	uint32_t file_size = inode->i_size;

	/* オフセットがファイルサイズを超えている */
	if (offset >= file_size) {
		if (out_len)
			*out_len = 0;
		return 0;
	}

	/* 読み取るバイト数を調整 */
	uint32_t bytes_to_read = len;
	if (offset + bytes_to_read > file_size)
		bytes_to_read = file_size - offset;

	uint32_t bytes_read = 0;
	uint32_t current_offset = offset;

	while (bytes_read < bytes_to_read) {
		/* 現在のブロックインデックスとブロック内オフセット */
		uint32_t block_idx = current_offset / sb->block_size;
		uint32_t block_off = current_offset % sb->block_size;

		/* ブロック番号を取得 */
		uint32_t block_num;
		int r = ext2_get_block_num(sb, inode, block_idx, &block_num);
		if (r != 0 || block_num == 0)
			break;

		/* ブロックデータを読み取る */
		uint8_t block_data[4096];
		if (sb->cache) {
			if (block_cache_read(sb->cache, block_num,
					     block_data) != 0)
				break;
		} else {
			uint32_t block_offset = block_num * sb->block_size;
			if (block_offset + sb->block_size > sb->image_size)
				break;
			mem_copy(block_data, sb->image + block_offset,
				 sb->block_size);
		}

		/* このブロックから読み取るバイト数 */
		uint32_t copy_size = sb->block_size - block_off;
		if (bytes_read + copy_size > bytes_to_read)
			copy_size = bytes_to_read - bytes_read;

		mem_copy((uint8_t *)buf + bytes_read, block_data + block_off,
			 copy_size);
		bytes_read += copy_size;
		current_offset += copy_size;
	}

	if (out_len)
		*out_len = bytes_read;

	return 0;
}

/* ------- 書き込み関連の簡易実装（直接ブロックのみ対応） ------- */

/* 空きブロックを割り当てる（簡易実装） */
int ext2_allocate_block(struct ext2_super *sb, uint32_t *out_block) {
	if (!sb || !sb->cache || !out_block)
		return -1;

	uint32_t bs = sb->block_size;
	uint32_t blocks_per_group = sb->sb.s_blocks_per_group;

	for (uint32_t g = 0; g < sb->num_groups; ++g) {
		uint32_t gd_block_num = sb->group_desc_offset + (g * 32) / bs;
		uint32_t gd_off = (g * 32) % bs;

		uint8_t *gd_block = (uint8_t *)kmalloc(bs);
		if (!gd_block)
			return -2;
		if (block_cache_read(sb->cache, gd_block_num, gd_block) != 0) {
			kfree(gd_block);
			continue;
		}

		uint32_t block_bitmap = le32(gd_block + gd_off + 0);

		uint8_t *bm = (uint8_t *)kmalloc(bs);
		if (!bm) {
			kfree(gd_block);
			return -3;
		}
		if (block_cache_read(sb->cache, block_bitmap, bm) != 0) {
			kfree(bm);
			kfree(gd_block);
			continue;
		}

		for (uint32_t i = 0; i < blocks_per_group; ++i) {
			uint32_t byte_idx = i / 8;
			uint8_t bit_mask = (uint8_t)(1u << (i % 8));
			if ((bm[byte_idx] & bit_mask) == 0) {
				/* 割当て */
				bm[byte_idx] |= bit_mask;
				if (block_cache_write(sb->cache, block_bitmap,
						      bm) != 0) {
					kfree(bm);
					kfree(gd_block);
					return -4;
				}

				/* グループの空き数をデクリメント */
				uint16_t bg_free = le16(gd_block + gd_off + 12);
				if (bg_free > 0)
					bg_free -= 1;
				write_le16(gd_block + gd_off + 12, bg_free);
				if (block_cache_write(sb->cache, gd_block_num,
						      gd_block) != 0) {
					kfree(bm);
					kfree(gd_block);
					return -5;
				}

				/* スーパーブロックの空き数を更新して書き戻す（簡易） */
				if (sb->sb.s_free_blocks_count > 0)
					sb->sb.s_free_blocks_count -= 1;
				uint32_t sb_block_num = 1024 / bs;
				uint8_t *sb_block = (uint8_t *)kmalloc(bs);
				if (sb_block) {
					if (block_cache_read(sb->cache,
							     sb_block_num,
							     sb_block) == 0) {
						uint32_t sb_offset = 1024 % bs;
						write_le32(
							sb_block + sb_offset +
								12,
							sb->sb.s_free_blocks_count);
						block_cache_write(sb->cache,
								  sb_block_num,
								  sb_block);
					}
					kfree(sb_block);
				}

				uint32_t allocated = g * blocks_per_group + i +
						     sb->sb.s_first_data_block;
				*out_block = allocated;
				kfree(bm);
				kfree(gd_block);
				return 0;
			}
		}

		kfree(bm);
		kfree(gd_block);
	}

	return -6; /* 空きなし */
}

/* 空きinodeを割り当てる（内部使用、簡易実装） */
static int ext2_allocate_inode(struct ext2_super *sb, uint32_t *out_inode) {
	if (!sb || !sb->cache || !out_inode)
		return -1;

	uint32_t bs = sb->block_size;
	uint32_t inodes_per_group = sb->sb.s_inodes_per_group;

	for (uint32_t g = 0; g < sb->num_groups; ++g) {
		uint32_t gd_block_num = sb->group_desc_offset + (g * 32) / bs;
		uint32_t gd_off = (g * 32) % bs;

		uint8_t *gd_block = (uint8_t *)kmalloc(bs);
		if (!gd_block)
			return -2;
		if (block_cache_read(sb->cache, gd_block_num, gd_block) != 0) {
			kfree(gd_block);
			continue;
		}

		uint32_t inode_bitmap = le32(gd_block + gd_off + 4);

		uint8_t *bm = (uint8_t *)kmalloc(bs);
		if (!bm) {
			kfree(gd_block);
			return -3;
		}
		if (block_cache_read(sb->cache, inode_bitmap, bm) != 0) {
			kfree(bm);
			kfree(gd_block);
			continue;
		}

		for (uint32_t i = 0; i < inodes_per_group; ++i) {
			uint32_t byte_idx = i / 8;
			uint8_t bit_mask = (uint8_t)(1u << (i % 8));
			if ((bm[byte_idx] & bit_mask) == 0) {
				bm[byte_idx] |= bit_mask;
				if (block_cache_write(sb->cache, inode_bitmap,
						      bm) != 0) {
					kfree(bm);
					kfree(gd_block);
					return -4;
				}

				uint16_t bg_free_inodes =
					le16(gd_block + gd_off + 14);
				if (bg_free_inodes > 0)
					bg_free_inodes -= 1;
				write_le16(gd_block + gd_off + 14,
					   bg_free_inodes);
				if (block_cache_write(sb->cache, gd_block_num,
						      gd_block) != 0) {
					kfree(bm);
					kfree(gd_block);
					return -5;
				}

				if (sb->sb.s_free_inodes_count > 0)
					sb->sb.s_free_inodes_count -= 1;
				/* スーパーブロック書き戻し（簡易） */
				uint32_t sb_block_num = 1024 / bs;
				uint8_t *sb_block = (uint8_t *)kmalloc(bs);
				if (sb_block) {
					if (block_cache_read(sb->cache,
							     sb_block_num,
							     sb_block) == 0) {
						uint32_t sb_offset = 1024 % bs;
						write_le32(
							sb_block + sb_offset +
								16,
							sb->sb.s_free_inodes_count);
						block_cache_write(sb->cache,
								  sb_block_num,
								  sb_block);
					}
					kfree(sb_block);
				}

				uint32_t inode_num =
					g * inodes_per_group + i + 1;
				*out_inode = inode_num;
				kfree(bm);
				kfree(gd_block);
				return 0;
			}
		}

		kfree(bm);
		kfree(gd_block);
	}

	return -6; /* 空きなし */
}

/* inodeをディスクに書き込む（簡易実装） */
int ext2_write_inode(struct ext2_super *sb, uint32_t inode_num,
		     struct ext2_inode *inode) {
	if (!sb || !sb->cache || !inode || inode_num == 0)
		return -1;

	uint32_t inode_index = inode_num - 1;
	uint32_t group = inode_index / sb->sb.s_inodes_per_group;
	uint32_t local_index = inode_index % sb->sb.s_inodes_per_group;
	if (group >= sb->num_groups)
		return -2;

	uint32_t bs = sb->block_size;
	uint32_t gd_block_num = sb->group_desc_offset + (group * 32) / bs;
	uint32_t gd_off = (group * 32) % bs;

	uint8_t *gd_block = (uint8_t *)kmalloc(bs);
	if (!gd_block)
		return -3;
	if (block_cache_read(sb->cache, gd_block_num, gd_block) != 0) {
		kfree(gd_block);
		return -4;
	}

	uint32_t inode_table = le32(gd_block + gd_off + 8);
	uint32_t inode_size = (sb->sb.s_inode_size > 0) ? sb->sb.s_inode_size :
							  128;
	uint32_t inode_block_num =
		inode_table + (local_index * inode_size) / bs;
	uint32_t inode_offset_in_block = (local_index * inode_size) % bs;

	uint8_t *inode_block = (uint8_t *)kmalloc(bs);
	if (!inode_block) {
		kfree(gd_block);
		return -5;
	}
	if (block_cache_read(sb->cache, inode_block_num, inode_block) != 0) {
		kfree(inode_block);
		kfree(gd_block);
		return -6;
	}

	/* 書き込み */
	write_le16(inode_block + inode_offset_in_block + 0, inode->i_mode);
	write_le16(inode_block + inode_offset_in_block + 2, inode->i_uid);
	write_le32(inode_block + inode_offset_in_block + 4, inode->i_size);
	write_le32(inode_block + inode_offset_in_block + 8, inode->i_atime);
	write_le32(inode_block + inode_offset_in_block + 12, inode->i_ctime);
	write_le32(inode_block + inode_offset_in_block + 16, inode->i_mtime);
	write_le32(inode_block + inode_offset_in_block + 20, inode->i_dtime);
	write_le16(inode_block + inode_offset_in_block + 24, inode->i_gid);
	write_le16(inode_block + inode_offset_in_block + 26,
		   inode->i_links_count);
	write_le32(inode_block + inode_offset_in_block + 28, inode->i_blocks);
	write_le32(inode_block + inode_offset_in_block + 32, inode->i_flags);
	write_le32(inode_block + inode_offset_in_block + 36, inode->i_osd1);
	for (int i = 0; i < 15; ++i) {
		write_le32(inode_block + inode_offset_in_block + 40 + i * 4,
			   inode->i_block[i]);
	}
	write_le32(inode_block + inode_offset_in_block + 100,
		   inode->i_generation);
	write_le32(inode_block + inode_offset_in_block + 104,
		   inode->i_file_acl);
	write_le32(inode_block + inode_offset_in_block + 108, inode->i_dir_acl);
	write_le32(inode_block + inode_offset_in_block + 112, inode->i_faddr);

	int w = block_cache_write(sb->cache, inode_block_num, inode_block);
	kfree(inode_block);
	kfree(gd_block);
	return (w == 0) ? 0 : -7;
}

/* inodeのデータを書き込む（簡易実装: 直接ブロックのみ） */
int ext2_write_inode_data(struct ext2_super *sb, struct ext2_inode *inode,
			  const void *buf, size_t len, uint32_t offset,
			  size_t *out_len) {
	if (!sb || !sb->cache || !inode || !buf)
		return -1;

	uint32_t bs = sb->block_size;
	uint32_t bytes_written = 0;
	uint32_t current_offset = offset;

	while (bytes_written < len) {
		uint32_t block_idx = current_offset / bs;
		uint32_t block_off = current_offset % bs;
		if (block_idx >= 12) {
			/* 直接ブロックのみ対応 */
			break;
		}

		uint32_t block_num = inode->i_block[block_idx];
		if (block_num == 0) {
			uint32_t newb;
			if (ext2_allocate_block(sb, &newb) != 0)
				break;
			inode->i_block[block_idx] = newb;
			inode->i_blocks += bs / 512;
			block_num = newb;
		}

		uint8_t *bbuf = (uint8_t *)kmalloc(bs);
		if (!bbuf)
			break;
		if (block_cache_read(sb->cache, block_num, bbuf) != 0) {
			for (uint32_t i = 0; i < bs; ++i)
				bbuf[i] = 0;
		}

		uint32_t to_copy = bs - block_off;
		if (to_copy > (len - bytes_written))
			to_copy = (uint32_t)(len - bytes_written);

		for (uint32_t i = 0; i < to_copy; ++i)
			bbuf[block_off + i] =
				((const uint8_t *)buf)[bytes_written + i];

		if (block_cache_write(sb->cache, block_num, bbuf) != 0) {
			kfree(bbuf);
			break;
		}
		kfree(bbuf);

		bytes_written += to_copy;
		current_offset += to_copy;
	}

	uint32_t new_size = offset + bytes_written;
	if (new_size > inode->i_size)
		inode->i_size = new_size;
	if (out_len)
		*out_len = bytes_written;
	return 0;
}

/* ブロック取得または割当て（簡易: 直接ブロックのみ） */
int ext2_get_or_alloc_block(struct ext2_super *sb, struct ext2_inode *inode,
			    uint32_t block_index, uint32_t *out_block_num) {
	if (!sb || !sb->cache || !inode || !out_block_num)
		return -1;

	if (block_index < 12) {
		uint32_t b = inode->i_block[block_index];
		if (b == 0) {
			uint32_t newb;
			if (ext2_allocate_block(sb, &newb) != 0)
				return -2;
			inode->i_block[block_index] = newb;
			*out_block_num = newb;
			return 0;
		}
		*out_block_num = b;
		return 0;
	}
	return -3; /* 非対応 */
}

/* ルート直下にファイルを作成（簡易実装） */
int ext2_create_file(struct ext2_super *sb, const char *path, uint16_t mode,
		     uint32_t *out_inode_num) {
	if (!sb || !sb->cache || !path || !out_inode_num)
		return -1;
	/* パスは "/name" 形式のみ対応 */
	if (path[0] != '/')
		return -2;
	const char *name = path + 1;
	if (name[0] == '\0')
		return -3;
	for (const char *p = name; *p; ++p) {
		if (*p == '/')
			return -3;
	}

	/* 既存ファイルチェック */
	struct ext2_inode root_inode;
	if (ext2_read_inode(sb, EXT2_ROOT_INO, &root_inode) != 0)
		return -4;
	uint32_t existing;
	if (ext2_find_file_in_dir(sb, &root_inode, name, &existing) == 0)
		return -5; /* 既に存在 */

	uint32_t new_inode_num;
	if (ext2_allocate_inode(sb, &new_inode_num) != 0)
		return -6;

	struct ext2_inode new_inode;
	mem_set(&new_inode, 0, sizeof(new_inode));
	new_inode.i_mode = mode;
	new_inode.i_uid = 0;
	new_inode.i_gid = 0;
	new_inode.i_size = 0;
	new_inode.i_links_count = 1;
	new_inode.i_blocks = 0;

	if (ext2_write_inode(sb, new_inode_num, &new_inode) != 0)
		return -7;

	/* ルートディレクトリの最初のブロックにエントリを追加する */
	uint32_t block_num = root_inode.i_block[0];
	if (block_num == 0) {
		uint32_t nb;
		if (ext2_allocate_block(sb, &nb) != 0)
			return -8;
		root_inode.i_block[0] = nb;
		root_inode.i_blocks += sb->block_size / 512;
		root_inode.i_size += sb->block_size;
	}

	uint8_t *block = (uint8_t *)kmalloc(sb->block_size + 4);
	if (!block)
		return -9;
	if (block_cache_read(sb->cache, root_inode.i_block[0], block) != 0) {
		/* 新規ブロックはゼロクリア */
		for (uint32_t i = 0; i < sb->block_size; ++i)
			block[i] = 0;
	}

	uint16_t name_len = (uint16_t)str_len(name);
	uint16_t needed = (uint16_t)(((8 + name_len + 3) / 4) * 4);

	uint32_t offset = 0;
	uint32_t last_offset = 0;
	uint16_t last_rec_len = 0;
	uint8_t last_name_len = 0;
	int found_slot = 0;

	while (offset < sb->block_size) {
		uint32_t ino = le32(block + offset + 0);
		uint16_t rec_len = le16(block + offset + 4);
		uint8_t nlen = block[offset + 6];
		if (rec_len == 0)
			break;
		if (ino == 0 && rec_len >= needed) {
			/* 空きエントリを利用 */
			write_le32(block + offset + 0, new_inode_num);
			write_le16(block + offset + 4, rec_len);
			block[offset + 6] = (uint8_t)name_len;
			block[offset + 7] = EXT2_FT_REG_FILE;
			for (int i = 0; i < name_len; ++i)
				block[offset + 8 + i] = name[i];
			found_slot = 1;
			break;
		}
		last_offset = offset;
		last_rec_len = rec_len;
		last_name_len = nlen;
		offset += rec_len;
	}

	if (!found_slot) {
		/* 最後のエントリを分割できるか試す */
		if (last_rec_len == 0 ||
		    last_offset + last_rec_len > sb->block_size) {
			kfree(block);
			return -10;
		}
		uint16_t minimal_last =
			(uint16_t)(((8 + last_name_len + 3) / 4) * 4);
		if (last_rec_len >= minimal_last + needed) {
			/* 最後のエントリのrec_lenを縮小して新しいエントリを作る */
			write_le16(block + last_offset + 4, minimal_last);
			uint32_t new_off = last_offset + minimal_last;
			uint16_t new_rec = last_rec_len - minimal_last;
			write_le32(block + new_off + 0, new_inode_num);
			write_le16(block + new_off + 4, new_rec);
			block[new_off + 6] = (uint8_t)name_len;
			block[new_off + 7] = EXT2_FT_REG_FILE;
			for (int i = 0; i < name_len; ++i)
				block[new_off + 8 + i] = name[i];
			found_slot = 1;
		}
	}

	if (!found_slot) {
		kfree(block);
		return -11;
	}

	if (block_cache_write(sb->cache, root_inode.i_block[0], block) != 0) {
		kfree(block);
		return -12;
	}
	kfree(block);

	/* ルートinodeを書き戻す */
	if (ext2_write_inode(sb, EXT2_ROOT_INO, &root_inode) != 0)
		return -13;

	*out_inode_num = new_inode_num;
	return 0;
}

/**
 * @brief シンボリックリンクを解決する
 */
int ext2_resolve_symlink(struct ext2_super *sb, struct ext2_inode *link_inode,
			 uint32_t *out_target) {
	if (!sb || !link_inode || !out_target)
		return -1;

	/* シンボリックリンクでない場合はエラー */
	if ((link_inode->i_mode & 0xF000) != EXT2_S_IFLNK)
		return -2;

	char target_path[256];
	size_t target_len = link_inode->i_size;

	if (target_len >= sizeof(target_path))
		return -3;

	/* シンボリックリンクの内容を読み取る */
	/* 短いリンク（60バイト以下）はi_blockに直接格納される */
	if (target_len <= 60) {
		mem_copy(target_path, (const char *)link_inode->i_block,
			 target_len);
	} else {
		/* 長いリンクはブロックに格納される */
		size_t read_len;
		int r = ext2_read_inode_data(sb, link_inode, target_path,
					     target_len, 0, &read_len);
		if (r != 0 || read_len != target_len)
			return -4;
	}
	target_path[target_len] = '\0';

	/* リンク先のパスを解決 */
	return ext2_resolve_path(sb, target_path, out_target);
}

/**
 * @brief パスからinode番号を解決する（完全なパス対応）
 */
int ext2_resolve_path(struct ext2_super *sb, const char *path,
		      uint32_t *out_inode) {
	if (!sb || !path || !out_inode)
		return -1;

	/* 空のパスはエラー */
	if (path[0] == '\0')
		return -2;

	uint32_t current_inode;

	/* 絶対パスの場合はルートから開始 */
	if (path[0] == '/') {
		current_inode = EXT2_ROOT_INO;
		path++; /* '/' をスキップ */
	} else {
		/* 相対パスの場合はルートから開始（カレントディレクトリ未実装） */
		current_inode = EXT2_ROOT_INO;
	}

	/* パスが空になった（"/" のみ）場合はルートを返す */
	if (path[0] == '\0') {
		*out_inode = current_inode;
		return 0;
	}

	/* パスコンポーネントを順次解決 */
	char component[256];
	int comp_idx = 0;
	int symlink_depth = 0;
	const int MAX_SYMLINK_DEPTH = 8;

	while (1) {
		/* 次のコンポーネントを取得 */
		comp_idx = 0;
		while (*path && *path != '/' && comp_idx < 255) {
			component[comp_idx++] = *path++;
		}
		component[comp_idx] = '\0';

		/* 末尾の '/' をスキップ */
		while (*path == '/')
			path++;

		/* コンポーネントが空の場合（連続する '/'）はスキップ */
		if (comp_idx == 0) {
			if (*path == '\0')
				break;
			continue;
		}

		/* 現在のinodeを読み取る */
		struct ext2_inode inode;
		int r = ext2_read_inode(sb, current_inode, &inode);
		if (r != 0)
			return r;

		/* シンボリックリンクの場合は解決 */
		if ((inode.i_mode & 0xF000) == EXT2_S_IFLNK) {
			if (++symlink_depth > MAX_SYMLINK_DEPTH)
				return -10; /* シンボリックリンクループ */

			uint32_t target_inode;
			r = ext2_resolve_symlink(sb, &inode, &target_inode);
			if (r != 0)
				return r;

			current_inode = target_inode;
			r = ext2_read_inode(sb, current_inode, &inode);
			if (r != 0)
				return r;
		}

		/* ディレクトリでない場合はエラー */
		if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR)
			return -3;

		/* "." は現在のディレクトリ */
		if (comp_idx == 1 && component[0] == '.') {
			/* current_inodeは変更なし */
		}
		/* ".." は親ディレクトリ */
		else if (comp_idx == 2 && component[0] == '.' &&
			 component[1] == '.') {
			/* TODO: 親ディレクトリの検索（現在は未実装） */
			if (current_inode != EXT2_ROOT_INO) {
				/* 親ディレクトリを見つける必要がある */
				/* 簡易実装：ルートを返す */
				current_inode = EXT2_ROOT_INO;
			}
		}
		/* 通常のコンポーネント */
		else {
			uint32_t next_inode;
			r = ext2_find_file_in_dir(sb, &inode, component,
						  &next_inode);
			if (r != 0)
				return r;

			current_inode = next_inode;
		}

		/* パスの末尾に達した */
		if (*path == '\0')
			break;
	}

	*out_inode = current_inode;
	return 0;
}

/**
 * @brief パスからファイルを読み取る（完全版）
 */
int ext2_read_file_by_path(struct ext2_super *sb, const char *path, void *buf,
			   size_t len, uint32_t offset, size_t *out_len) {
	if (!sb || !path || !buf)
		return -1;

	/* パスからinode番号を解決 */
	uint32_t inode_num;
	int r = ext2_resolve_path(sb, path, &inode_num);
	if (r != 0)
		return r;

	/* inodeを読み取る */
	struct ext2_inode inode;
	r = ext2_read_inode(sb, inode_num, &inode);
	if (r != 0)
		return r;

	/* シンボリックリンクの場合は解決 */
	if ((inode.i_mode & 0xF000) == EXT2_S_IFLNK) {
		uint32_t target_inode;
		r = ext2_resolve_symlink(sb, &inode, &target_inode);
		if (r != 0)
			return r;

		r = ext2_read_inode(sb, target_inode, &inode);
		if (r != 0)
			return r;
	}

	/* 通常ファイルでない場合はエラー */
	if ((inode.i_mode & 0xF000) != EXT2_S_IFREG)
		return -2;

	/* ファイルデータを読み取る */
	return ext2_read_inode_data(sb, &inode, buf, len, offset, out_len);
}

/**
 * @brief ディレクトリの内容を一覧表示する（デバッグ用）
 */
int ext2_list_dir(struct ext2_super *sb, struct ext2_inode *dir_inode) {
	if (!sb || !dir_inode)
		return -1;

	/* ディレクトリでない場合はエラー */
	if ((dir_inode->i_mode & 0xF000) != EXT2_S_IFDIR)
		return -2;

	uint32_t dir_size = dir_inode->i_size;
	uint32_t read_offset = 0;
	uint32_t block_idx = 0;

	while (read_offset < dir_size) {
		/* ブロック番号を取得 */
		uint32_t block_num;
		int r = ext2_get_block_num(sb, dir_inode, block_idx,
					   &block_num);
		if (r != 0 || block_num == 0)
			break;

		/* ブロックデータを読み取る（キャッシュ対応） */
		uint8_t block_data[4096]; /* 最大ブロックサイズ */
		if (sb->cache) {
			if (block_cache_read(sb->cache, block_num,
					     block_data) != 0)
				break;
		} else {
			uint32_t block_offset = block_num * sb->block_size;
			if (block_offset + sb->block_size > sb->image_size)
				break;
			const uint8_t *src = sb->image + block_offset;
			for (uint32_t i = 0; i < sb->block_size; i++) {
				block_data[i] = src[i];
			}
		}

		uint32_t offset = 0;

		while (offset < sb->block_size && read_offset < dir_size) {
			const uint8_t *entry = block_data + offset;
			uint32_t inode = le32(entry + 0);
			uint16_t rec_len = le16(entry + 4);
			uint8_t name_len = entry[6];
			uint8_t file_type = entry[7];

			if (rec_len == 0)
				break;

			if (inode != 0 && name_len > 0) {
				char name[256];
				mem_set(name, 0, sizeof(name));
				for (uint8_t i = 0; i < name_len && i < 255;
				     i++) {
					name[i] = entry[8 + i];
				}

				const char *type_str = "UNKNOWN";
				if (file_type == EXT2_FT_REG_FILE)
					type_str = "FILE";
				else if (file_type == EXT2_FT_DIR)
					type_str = "DIR";
				else if (file_type == EXT2_FT_SYMLINK)
					type_str = "SYMLINK";

				/* inodeを読み取ってサイズを取得 */
				struct ext2_inode file_inode;
				uint32_t file_size = 0;
				if (ext2_read_inode(sb, inode, &file_inode) ==
				    0) {
					file_size = file_inode.i_size;
				}

				printk("  %-20s [%-7s] size: %u\n", name,
				       type_str, inode, file_size);
			}

			offset += rec_len;
			read_offset += rec_len;
		}

		block_idx++;
	}

	return 0;
}

/**
 * @brief ext2ファイルシステムをブロックキャッシュ経由でマウントする
 */
int ext2_mount_with_cache(struct block_cache *cache, struct ext2_super **out) {
	if (!cache || !out) {
		return -1;
	}
	/* ブロックキャッシュのブロックサイズに合わせたバッファを確保して
	 * スーパーブロックが存在する（イメージ先頭から）オフセット1024を含む
	 * ブロックを読み込む。
	 */
	uint32_t bs = cache->block_size;
	uint32_t sb_block_num =
		1024 / bs; /* スーパーブロックを含むブロック番号 */
	uint32_t sb_offset_in_block = 1024 % bs;

	uint8_t *sb_buf = (uint8_t *)kmalloc(bs);
	if (!sb_buf)
		return -2;

	if (block_cache_read(cache, sb_block_num, sb_buf) != 0) {
		kfree(sb_buf);
		return -3;
	}

	/* マジックナンバーチェック（スーパーブロックの先頭から56バイト目） */
	uint16_t magic = le16(sb_buf + sb_offset_in_block + 56);
	if (magic != EXT2_SUPER_MAGIC) {
		kfree(sb_buf);
		return -4; /* ext2ではない */
	}

	/* ext2_superを確保 */
	struct ext2_super *sb =
		(struct ext2_super *)kmalloc(sizeof(struct ext2_super));
	if (!sb) {
		kfree(sb_buf);
		return -5;
	}

	mem_set(sb, 0, sizeof(struct ext2_super));
	sb->image = NULL; /* キャッシュ経由なので不要 */
	sb->image_size = 0;
	sb->cache = cache;

	/* スーパーブロックのフィールドを読み取る */
	const uint8_t *sb_data = sb_buf + sb_offset_in_block;
	sb->sb.s_inodes_count = le32(sb_data + 0);
	sb->sb.s_blocks_count = le32(sb_data + 4);
	sb->sb.s_r_blocks_count = le32(sb_data + 8);
	sb->sb.s_free_blocks_count = le32(sb_data + 12);
	sb->sb.s_free_inodes_count = le32(sb_data + 16);
	sb->sb.s_first_data_block = le32(sb_data + 20);
	sb->sb.s_log_block_size = le32(sb_data + 24);
	sb->sb.s_log_frag_size = le32(sb_data + 28);
	sb->sb.s_blocks_per_group = le32(sb_data + 32);
	sb->sb.s_frags_per_group = le32(sb_data + 36);
	sb->sb.s_inodes_per_group = le32(sb_data + 40);

	/* 拡張フィールド */
	sb->sb.s_first_ino = le32(sb_data + 84);
	sb->sb.s_inode_size = le16(sb_data + 88);

	/* ブロックサイズを計算 */
	sb->block_size = 1024 << sb->sb.s_log_block_size;

	/* ブロックグループ数を計算 */
	sb->num_groups =
		(sb->sb.s_blocks_count + sb->sb.s_blocks_per_group - 1) /
		sb->sb.s_blocks_per_group;

	/* グループディスクリプタテーブルの開始ブロック番号を設定 */
	uint32_t sb_block = (sb->sb.s_first_data_block == 0) ? 1 : 2;
	sb->group_desc_offset = sb_block; /* ブロック番号として保持 */

	/* 一時バッファを解放して結果を返す */
	kfree(sb_buf);

	*out = sb;
	return 0;
}
