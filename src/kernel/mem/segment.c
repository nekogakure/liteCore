#include <util/config.h>
#include <stdint.h>
#include <stddef.h>
#include <mem/segment.h>

/* NULL, kernel code, kernel data, user code 32-bit, user data, user code 64-bit, TSS(2エントリ) = 8 */
struct gdt_entry gdt_entries[8];
struct gdt_ptr gp;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access,
			 uint8_t gran) {
	gdt_entries[num].base_low = (base & 0xFFFF);
	gdt_entries[num].base_middle = (base >> 16) & 0xFF;
	gdt_entries[num].base_high = (base >> 24) & 0xFF;

	gdt_entries[num].limit_low = (limit & 0xFFFF);
	gdt_entries[num].granularity = (limit >> 16) & 0x0F;

	gdt_entries[num].granularity |= (gran & 0xF0);
	gdt_entries[num].access = access;
}

void gdt_build() {
	/* 
	 * 0x00: NULL
	 * 0x08: Kernel Code (64-bit)
	 * 0x10: Kernel Data
	 * 0x18: User Code (32-bit) - for SYSRET compatibility
	 * 0x20: User Data
	 * 0x28: User Code (64-bit) - actual user code segment
	 */
	gdt_set_gate(0, 0, 0, 0, 0); /* NULL descriptor */
	gdt_set_gate(1, 0x0, 0xFFFFF, 0x9A,
		     0xAF); /* 64-bit kernel code: 0xAF = Long mode */
	gdt_set_gate(
		2, 0x0, 0xFFFFF, 0x92,
		0xCF); /* kernel data: 0xCF = 32-bit (L-bit must be 0 for data) */
	gdt_set_gate(3, 0x0, 0xFFFFF, 0xFA,
		     0xCF); /* 32-bit user code (for SYSRET) */
	gdt_set_gate(
		4, 0x0, 0xFFFFF, 0xF2,
		0xCF); /* user data: 0xCF = 32-bit (L-bit must be 0 for data) */
	gdt_set_gate(5, 0x0, 0xFFFFF, 0xFA, 0xAF); /* 64-bit user code: L=1 */
	gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
	gp.base = (uint64_t)&gdt_entries;
}

void gdt_install();

#if 0 // デバッグ用 - 必要に応じて有効化
void gdt_dump(void) {
	extern void printk(const char *fmt, ...);
	printk("[GDT DUMP] gp.base=0x%016lx gp.limit=0x%04x\n", gp.base,
	       gp.limit);
	for (int i = 0; i < 5; i++) {
		unsigned char *b = (unsigned char *)&gdt_entries[i];
		printk("gdt[%d]: ", i);
		for (size_t j = 0; j < sizeof(struct gdt_entry); j++) {
			printk("%02x", b[j]);
			if (j != sizeof(struct gdt_entry) - 1)
				printk(":");
		}
		printk("\n");
	}
}
#endif
