#ifndef _MEM_SEGMENT_H
#define _MEM_SEGMENT_H

#include <stdint.h>

/* GDTエントリ構造体 */
struct gdt_entry {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_middle;
	uint8_t access;
	uint8_t granularity;
	uint8_t base_high;
} __attribute__((packed));

/* 64-bit GDT pointer */
struct gdt_ptr {
	uint16_t limit;
	uint64_t base; // 64-bit base address
} __attribute__((packed));

void gdt_build();
void gdt_install();
void gdt_dump();
void gdt_install_lgdt();
void gdt_install_jump();

#endif /* _MEM_SEGMENT_H */