// Minimal IRQ trace buffer for lightweight recording from IRQ context.
#ifndef IRQ_TRACE_H
#define IRQ_TRACE_H

#include <stdint.h>

#define IRQ_TRACE_CAPACITY 1024

typedef struct irq_trace_entry {
	uint64_t vec;
	uint64_t rip;
	uint64_t rsp;
	uint64_t cr3;
	uint64_t rflags;
	uint32_t tid;
} irq_trace_entry_t;

void irq_trace_record(uint64_t vec, uint64_t rip, uint64_t rsp, uint64_t cr3,
		      uint64_t rflags, uint32_t tid);
void irq_trace_record_from_stack(uint64_t *regs_stack, uint32_t vec);
void irq_trace_dump(void);

#endif // IRQ_TRACE_H
