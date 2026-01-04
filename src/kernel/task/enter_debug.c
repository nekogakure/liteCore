#include <stdint.h>

extern void printk(const char *fmt, ...);

void log_usermode_frame(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t ss,
			uint64_t rflags, uint64_t cs) {
	printk("USERENTRY: RIP=0x%016lx RSP=0x%016lx CR3=0x%016lx SS=0x%lx RFLAGS=0x%016lx CS=0x%lx\n",
	       (unsigned long)rip, (unsigned long)rsp, (unsigned long)cr3,
	       (unsigned long)ss, (unsigned long)rflags, (unsigned long)cs);
}
