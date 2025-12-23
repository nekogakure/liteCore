#include <mem/tss.h>
#include <mem/segment.h>
#include <stdint.h>
#include <stddef.h>
#include <util/config.h>

extern void printk(const char *fmt, ...);

// グローバルTSS
static tss_entry_t tss __attribute__((aligned(4096)));

// 外部からアクセス可能なGDT関連の関数・変数
extern struct gdt_entry gdt_entries[];
extern struct gdt_ptr gp;
extern void gdt_install_lgdt(void);

/**
 * @brief GDTにTSSディスクリプタを設定
 */
static void gdt_set_tss(int num, uint64_t base, uint32_t limit, uint8_t access,
			uint8_t gran) {
	// 64ビットモードではTSSディスクリプタは16バイト（2エントリ）
	struct gdt_entry *entry = &gdt_entries[num];

	// 下位8バイト
	entry->limit_low = (limit & 0xFFFF);
	entry->base_low = (base & 0xFFFF);
	entry->base_middle = (base >> 16) & 0xFF;
	entry->access = access;
	entry->granularity = (limit >> 16) & 0x0F;
	entry->granularity |= (gran & 0xF0);
	entry->base_high = (base >> 24) & 0xFF;

	// 上位8バイト（64ビット拡張）
	struct gdt_entry *entry_high = &gdt_entries[num + 1];
	entry_high->limit_low = (base >> 32) & 0xFFFF;
	entry_high->base_low = (base >> 48) & 0xFFFF;
	entry_high->base_middle = 0;
	entry_high->access = 0;
	entry_high->granularity = 0;
	entry_high->base_high = 0;
}

/**
 * @brief TSSを初期化
 */
void tss_init(void) {
	// TSSをゼロクリア
	uint8_t *tss_bytes = (uint8_t *)&tss;
	for (size_t i = 0; i < sizeof(tss_entry_t); i++) {
		tss_bytes[i] = 0;
	}

	// I/O Permission Bitmapのオフセットを設定（使用しない場合はsizeofを設定）
	tss.iopb_offset = sizeof(tss_entry_t);

	// GDTを再構築してTSSディスクリプタを追加
	// 既存: NULL(0), kernel code(1), kernel data(2), user code 32-bit(3), user data(4), user code 64-bit(5)
	// 追加: TSS(6-7) - 64ビットTSSは2エントリを占有
	uint64_t tss_base = (uint64_t)(uintptr_t)&tss;
	uint32_t tss_limit = sizeof(tss_entry_t) - 1;

	// TSSディスクリプタを設定（アクセス権: 0x89 = Present, DPL=0, Type=Available TSS）
	gdt_set_tss(6, tss_base, tss_limit, 0x89, 0x00);

	// GDTのリミットを更新（8エントリ分 = NULL + kcode + kdata + ucode32 + udata + ucode64 + TSS(2))
	gp.limit = (sizeof(struct gdt_entry) * 8) - 1;

	// GDTをリロード
	gdt_install_lgdt();

	// TSSをロード（セレクタ = 6 * 8 = 0x30）
	asm volatile("ltr %%ax" : : "a"(0x30));

#ifdef INIT_MSG
	printk("tss_init: TSS initialized at 0x%016lx, selector=0x30\n",
	       tss_base);
#endif
}

/**
 * @brief カーネルスタックポインタを設定
 * @param stack カーネルスタックのトップアドレス
 */
void tss_set_kernel_stack(uint64_t stack) {
	tss.rsp0 = stack;
}
