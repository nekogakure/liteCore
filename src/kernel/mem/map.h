#ifndef _MEM_MAP_H
#define _MEM_MAP_H

#include <util/config.h>

// 4KB
#define FRAME_SIZE 4096

// メモリマップ (64-bit 対応)
typedef struct memmap {
	uint64_t start_addr; // 管理開始バイトアドレス
	uint64_t end_addr; // 管理終了バイトアドレス（exclusive）
	uint64_t start_frame; // 管理開始フレーム番号
	uint64_t frames; // 管理するフレーム数
	uint64_t max_frames; // ビットマップが表現可能な最大フレーム数
	uint32_t *bitmap; // ビットマップ配列へのポインタ
} memmap_t;

/* 64-bit 対応の API */
void memmap_init(uint64_t start, uint64_t end);
void *alloc_frame(void);
void free_frame(void *addr);
uint64_t frame_count(void);
void memmap_reserve(uint64_t start, uint64_t end);
const memmap_t *memmap_get(void);

#endif /* _MEM_MAP_H */
