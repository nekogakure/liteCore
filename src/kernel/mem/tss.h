#ifndef _MEM_TSS_H
#define _MEM_TSS_H

#include <stdint.h>

/**
 * @brief 64ビットモード用のTSS構造体
 */
typedef struct {
	uint32_t reserved0;
	uint64_t rsp0; // Ring 0 (カーネルモード) のスタックポインタ
	uint64_t rsp1; // Ring 1 のスタックポインタ（通常未使用）
	uint64_t rsp2; // Ring 2 のスタックポインタ（通常未使用）
	uint64_t reserved1;
	uint64_t ist1; // Interrupt Stack Table 1
	uint64_t ist2;
	uint64_t ist3;
	uint64_t ist4;
	uint64_t ist5;
	uint64_t ist6;
	uint64_t ist7;
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iopb_offset; // I/O Permission Bitmap offset
} __attribute__((packed)) tss_entry_t;

void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);

#endif /* _MEM_TSS_H */
