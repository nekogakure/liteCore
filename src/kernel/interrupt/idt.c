#include <util/config.h>
#include <util/console.h>
#include <interrupt/idt.h>
#include <interrupt/irq.h>

/* ELF 呼び出しスナップショット（elf.c でセットされる） */
extern volatile uint64_t elf_call_snapshot_func_addr;
extern volatile uint64_t elf_call_snapshot_rdi;
extern volatile uint64_t elf_call_snapshot_rsi;
extern volatile uint64_t elf_call_snapshot_rdx;
extern volatile uint64_t elf_call_snapshot_rsp;

/* PIC ports */
#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

static inline uint8_t inb(uint16_t port) {
	uint8_t ret;
	__asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
	return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Remap PIC to vectors starting at 32 */
static void pic_remap(void) {
	unsigned char a1, a2;
	a1 = inb(PIC1_DATA);
	a2 = inb(PIC2_DATA);

	outb(PIC1_COMMAND, 0x11);
	outb(PIC2_COMMAND, 0x11);
	outb(PIC1_DATA, 0x20); /* Master PIC vector offset */
	outb(PIC2_DATA, 0x28); /* Slave PIC vector offset */
	outb(PIC1_DATA, 0x04);
	outb(PIC2_DATA, 0x02);
	outb(PIC1_DATA, 0x01);
	outb(PIC2_DATA, 0x01);

	outb(PIC1_DATA, a1);
	outb(PIC2_DATA, a2);
}

extern void load_idt(void *ptr, unsigned size);

/* 64-bit IDT entry struct (16 bytes) */
struct idt_entry {
	uint16_t base_lo; // Offset bits 0-15
	uint16_t sel; // Segment selector
	uint8_t ist; // Interrupt Stack Table (usually 0)
	uint8_t flags; // Type and DPL
	uint16_t base_mid; // Offset bits 16-31
	uint32_t base_hi; // Offset bits 32-63
	uint32_t reserved; // Reserved (must be 0)
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base; // 64-bit base address
} __attribute__((packed));

extern void isr_stub_table(void); /* assembly stubs */
extern void isr14(void);

#define IDT_ENTRIES 256
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

static void idt_set_gate(int n, uint64_t handler) {
	idt[n].base_lo = handler & 0xFFFF;
	idt[n].sel =
		0x08; // code segment (was 0x38 in 64-bit, but 0x08 is standard)
	idt[n].ist = 0; // No separate interrupt stack
	idt[n].flags = 0x8E; // Present, DPL=0, Type=Interrupt Gate
	idt[n].base_mid = (handler >> 16) & 0xFFFF;
	idt[n].base_hi = (handler >> 32) & 0xFFFFFFFF;
	idt[n].reserved = 0;
}

/**
 * @fn irq_handler_c
 * @param vec 割り込みベクター番号
 */
void irq_handler_c(uint32_t vec) {
	if (vec >= 32 && vec < 32 + 16) {
		uint32_t irq = vec - 32;

		// タイマー割り込み(IRQ 0 = vec 32)は直接ハンドラを呼んでからスケジューリング
		if (vec == 32) {
			// 登録されたタイマーハンドラを呼ぶ（timer_ticksをインクリメント）
			extern void uefi_timer_tick(uint32_t, void *);
			uefi_timer_tick(0, NULL);

			if (irq >= 8)
				outb(PIC2_COMMAND, 0x20);
			outb(PIC1_COMMAND, 0x20);
			return;
		}

		if (vec == 33) {
			uint8_t status = inb(0x64);
			uint8_t sc = 0;
			if ((status & 0x01) != 0) {
				sc = inb(0x60);
			}
			interrupt_raise((vec << 16) | (uint32_t)sc);

			if (irq >= 8)
				outb(PIC2_COMMAND, 0x20);
			outb(PIC1_COMMAND, 0x20);
			return;
		}

		interrupt_raise((vec << 16) | 0u);

		if (irq >= 8)
			outb(PIC2_COMMAND, 0x20);
		outb(PIC1_COMMAND, 0x20);
	} else if (vec >= 32) {
		interrupt_raise((vec << 16) | 0u);
	}
}

extern void page_fault_handler(uint32_t vec);
extern void page_fault_handler_ex(uint32_t vec, uint32_t error_code,
				  uint32_t eip);

static volatile int first_exception = 1;
static uint64_t saved_rip = 0;
static uint64_t saved_rsp = 0;
static uint64_t saved_cs = 0;
static uint32_t saved_vec = 0;
/* ISR stub がセットする、PUSH_ALL 後の RSP を保存するデバッグ変数 */
volatile uint64_t last_isr_stack = 0;

/**
 * @fn irq_exception_ex
 * @brief 例外発生時の拡張ハンドラ
 */
void irq_exception_ex(uint32_t vec, uint32_t error_code) {
	if (first_exception) {
		first_exception = 0;
		saved_vec = vec;
		uint64_t *stack_ptr = (uint64_t *)last_isr_stack;

		printk("ISR stack snapshot (first 24 qwords at %p):\n",
		       stack_ptr);
		for (int i = 0; i < 24; i++) {
			printk("  stack[%02d]=0x%016lx\n", i,
			       (unsigned long)stack_ptr[i]);
		}

		uint64_t maybe_rip = 0, maybe_cs = 0, maybe_rsp = 0;
		for (int idx = 17; idx <= 20; idx++) {
			uint64_t val = stack_ptr[idx];
			if (val != 0) {
				maybe_rip = val;
				maybe_cs = (idx + 1 <= 23) ?
						   stack_ptr[idx + 1] :
						   0;
				break;
			}
		}

		saved_rip = maybe_rip;
		saved_cs = maybe_cs;
		saved_rsp = 0; /* kernel-mode exception: SS/RSP not pushed */
	}

	const char *exception_names[] = { "Divide by Zero",
					  "Debug",
					  "NMI",
					  "Breakpoint",
					  "Overflow",
					  "Bound Range Exceeded",
					  "Invalid Opcode",
					  "Device Not Available",
					  "Double Fault",
					  "Coprocessor Segment Overrun",
					  "Invalid TSS",
					  "Segment Not Present",
					  "Stack-Segment Fault",
					  "General Protection Fault",
					  "Page Fault",
					  "Reserved",
					  "x87 FPU Error",
					  "Alignment Check",
					  "Machine Check",
					  "SIMD FP Exception",
					  "Virtualization Exception",
					  "Control Protection Exception" };
	const char *name = (saved_vec < 22) ? exception_names[saved_vec] :
					      "Unknown Exception";

	printk("\n!!! CPU EXCEPTION !!!\n");
	printk("Exception: %s (vector %u)\n", name, (unsigned)saved_vec);
	printk("Error code: 0x%x\n", (unsigned)error_code);

	if (vec == 14) {
		// Page Fault - print CR2
		uint64_t fault_addr;
		asm volatile("mov %%cr2, %0" : "=r"(fault_addr));
		printk("Page Fault at address: 0x%lx\n", fault_addr);
	}

	printk("FIRST EXCEPTION INFO:\n");
	printk("  RIP: 0x%lx\n", saved_rip);
	printk("  CS:  0x%lx\n", saved_cs);
	printk("  RSP: 0x%lx\n", saved_rsp);

	/* ELF 呼び出し直前のスナップショットが存在すれば表示する（デバッグ用） */
	printk("ELF: call-snapshot: func=0x%lx rdi=0x%lx rsi=0x%lx rdx=0x%lx rsp=0x%lx\n",
	       (unsigned long)elf_call_snapshot_func_addr,
	       (unsigned long)elf_call_snapshot_rdi,
	       (unsigned long)elf_call_snapshot_rsi,
	       (unsigned long)elf_call_snapshot_rdx,
	       (unsigned long)elf_call_snapshot_rsp);

	while (1) {
		asm volatile("hlt");
	}
}

/**
 * @fn idt_init
 * @brief IDTを初期化する
 */
void idt_init(void) {
	pic_remap();

	/* CPU例外ハンドラ (0-31) */
	extern void isr0(void);
	extern void isr1(void);
	extern void isr2(void);
	extern void isr3(void);
	extern void isr4(void);
	extern void isr5(void);
	extern void isr6(void);
	extern void isr7(void);
	extern void isr8(void);
	extern void isr9(void);
	extern void isr10(void);
	extern void isr11(void);
	extern void isr12(void);
	extern void isr13(void);
	extern void isr14(void);
	extern void isr15(void);
	extern void isr16(void);
	extern void isr17(void);
	extern void isr18(void);
	extern void isr19(void);
	extern void isr20(void);
	extern void isr21(void);
	extern void isr22(void);
	extern void isr23(void);
	extern void isr24(void);
	extern void isr25(void);
	extern void isr26(void);
	extern void isr27(void);
	extern void isr28(void);
	extern void isr29(void);
	extern void isr30(void);
	extern void isr31(void);

	/* 明示的に各isrシンボルをextern宣言し、それをIDTに登録する（脳筋だぜぇ〜ｗｗｗ */
	extern void isr32(void);
	extern void isr33(void);
	extern void isr34(void);
	extern void isr35(void);
	extern void isr36(void);
	extern void isr37(void);
	extern void isr38(void);
	extern void isr39(void);
	extern void isr40(void);
	extern void isr41(void);
	extern void isr42(void);
	extern void isr43(void);
	extern void isr44(void);
	extern void isr45(void);
	extern void isr46(void);
	extern void isr47(void);
	extern void isr48(void);

	extern void isr128(void);

	/* CPU例外を登録 */
	idt_set_gate(0, (uint64_t)isr0);
	idt_set_gate(1, (uint64_t)isr1);
	idt_set_gate(2, (uint64_t)isr2);
	idt_set_gate(3, (uint64_t)isr3);
	idt_set_gate(4, (uint64_t)isr4);
	idt_set_gate(5, (uint64_t)isr5);
	idt_set_gate(6, (uint64_t)isr6);
	idt_set_gate(7, (uint64_t)isr7);
	idt_set_gate(8, (uint64_t)isr8);
	idt_set_gate(9, (uint64_t)isr9);
	idt_set_gate(10, (uint64_t)isr10);
	idt_set_gate(11, (uint64_t)isr11);
	idt_set_gate(12, (uint64_t)isr12);
	idt_set_gate(13, (uint64_t)isr13);
	idt_set_gate(14, (uint64_t)isr14); /* page fault */
	idt_set_gate(15, (uint64_t)isr15);
	idt_set_gate(16, (uint64_t)isr16);
	idt_set_gate(17, (uint64_t)isr17);
	idt_set_gate(18, (uint64_t)isr18);
	idt_set_gate(19, (uint64_t)isr19);
	idt_set_gate(20, (uint64_t)isr20);
	idt_set_gate(21, (uint64_t)isr21);
	idt_set_gate(22, (uint64_t)isr22);
	idt_set_gate(23, (uint64_t)isr23);
	idt_set_gate(24, (uint64_t)isr24);
	idt_set_gate(25, (uint64_t)isr25);
	idt_set_gate(26, (uint64_t)isr26);
	idt_set_gate(27, (uint64_t)isr27);
	idt_set_gate(28, (uint64_t)isr28);
	idt_set_gate(29, (uint64_t)isr29);
	idt_set_gate(30, (uint64_t)isr30);
	idt_set_gate(31, (uint64_t)isr31);

	/* IRQハンドラ */
	idt_set_gate(32, (uint64_t)isr32);
	idt_set_gate(33, (uint64_t)isr33);
	idt_set_gate(34, (uint64_t)isr34);
	idt_set_gate(35, (uint64_t)isr35);
	idt_set_gate(36, (uint64_t)isr36);
	idt_set_gate(37, (uint64_t)isr37);
	idt_set_gate(38, (uint64_t)isr38);
	idt_set_gate(39, (uint64_t)isr39);
	idt_set_gate(40, (uint64_t)isr40);
	idt_set_gate(41, (uint64_t)isr41);
	idt_set_gate(42, (uint64_t)isr42);
	idt_set_gate(43, (uint64_t)isr43);
	idt_set_gate(44, (uint64_t)isr44);
	idt_set_gate(45, (uint64_t)isr45);
	idt_set_gate(46, (uint64_t)isr46);
	idt_set_gate(47, (uint64_t)isr47);
	idt_set_gate(48, (uint64_t)isr48); /* APIC Timer */
	/* syscall vector: allow user mode (DPL=3) */
	/* set gate with DPL=3 (0xEE flags) */
	{
		uint64_t handler = (uint64_t)isr128;
		idt[128].base_lo = handler & 0xFFFF;
		idt[128].sel = 0x08;
		idt[128].ist = 0;
		idt[128].flags = 0xEE; /* Present, DPL=3, Interrupt Gate */
		idt[128].base_mid = (handler >> 16) & 0xFFFF;
		idt[128].base_hi = (handler >> 32) & 0xFFFFFFFF;
		idt[128].reserved = 0;
	}

	idtp.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
	idtp.base = (uint64_t)&idt;
	load_idt(&idtp, sizeof(idtp));
}
