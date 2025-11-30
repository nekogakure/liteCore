#include "irq_trace.h"
#include <task/multi_task.h>
#include <stdint.h>
#include <util/console.h>

static irq_trace_entry_t irq_trace_buf[IRQ_TRACE_CAPACITY];
static volatile unsigned irq_trace_head = 0; // next write index

// IRQ-safe minimal record: no allocation, no printk
void irq_trace_record(uint64_t vec, uint64_t rip, uint64_t rsp, uint64_t cr3,
		      uint64_t rflags, uint32_t tid) {
	unsigned idx = irq_trace_head++;
	idx &= (IRQ_TRACE_CAPACITY - 1);
	irq_trace_buf[idx].vec = vec;
	irq_trace_buf[idx].rip = rip;
	irq_trace_buf[idx].rsp = rsp;
	irq_trace_buf[idx].cr3 = cr3;
	irq_trace_buf[idx].rflags = rflags;
	irq_trace_buf[idx].tid = tid;
}

// regs_stack layout assumed by irq stubs: see irq_stubs.asm
// index 15 = RIP, index 17 = RFLAGS. RSP we record as pointer-based value
void irq_trace_record_from_stack(uint64_t *regs_stack, uint32_t vec) {
	uint64_t rip = regs_stack[15];
	uint64_t rflags = regs_stack[17];
	// saved RSP pointer: caller's code in IRQ handler will expect saved RSP
	uint64_t rsp_ptr = (uint64_t)(&regs_stack[15]);

	uint64_t cr3_val = 0;
	asm volatile("mov %%cr3, %0" : "=r"(cr3_val));

	uint32_t tid = 0;
	task_t *t = task_current();
	if (t)
		tid = t->tid;

	irq_trace_record(vec, rip, rsp_ptr, cr3_val, rflags, tid);
}

// Dump the buffer using printk — call only from non-IRQ context
void irq_trace_dump(void) {
	printk("--- IRQ trace dump (last %d entries) ---\n",
	       IRQ_TRACE_CAPACITY);
	unsigned start = (irq_trace_head >= IRQ_TRACE_CAPACITY) ?
				 (irq_trace_head & (IRQ_TRACE_CAPACITY - 1)) :
				 0;
	unsigned count = (irq_trace_head >= IRQ_TRACE_CAPACITY) ?
				 IRQ_TRACE_CAPACITY :
				 irq_trace_head;
	for (unsigned i = 0; i < count; ++i) {
		unsigned idx = (start + i) & (IRQ_TRACE_CAPACITY - 1);
		irq_trace_entry_t *e = &irq_trace_buf[idx];
		printk("[%3u] vec=%u tid=%u rip=0x%lx rsp=0x%lx cr3=0x%lx rflags=0x%lx\n",
		       (unsigned)i, (unsigned)e->vec, (unsigned)e->tid,
		       (unsigned long)e->rip, (unsigned long)e->rsp,
		       (unsigned long)e->cr3, (unsigned long)e->rflags);
	}
}
