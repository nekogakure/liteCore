#include <syscall.h>
#include <util/console.h>
#include <task/multi_task.h>
#include <mem/manager.h>
#include <mem/paging.h>
#include <mem/map.h>
#include <device/keyboard.h>
#include <mem/usercopy.h>
#include <fs/vfs.h>
#include <stdint.h>

/* arch_prctl codes */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* MSR addresses for FS and GS base */
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_CSTAR 0xC0000083
#define MSR_SFMASK 0xC0000084

/* EFER bits */
#define EFER_SCE (1 << 0) /* System Call Extensions */

static inline void wrmsr(uint32_t msr, uint64_t value) {
	uint32_t low = (uint32_t)value;
	uint32_t high = (uint32_t)(value >> 32);
	asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
	uint32_t low, high;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

static uint64_t sys_arch_prctl(int code, uint64_t addr) {
	switch (code) {
	case ARCH_SET_FS:
		wrmsr(MSR_FS_BASE, addr);
		return 0;
	case ARCH_SET_GS:
		wrmsr(MSR_GS_BASE, addr);
		return 0;
	case ARCH_GET_FS:
		/* addr is a pointer to store the FS base */
		if (copy_to_user((void *)addr,
				 &(uint64_t){ rdmsr(MSR_FS_BASE) },
				 sizeof(uint64_t)) != 0)
			return (uint64_t)-1;
		return 0;
	case ARCH_GET_GS:
		/* addr is a pointer to store the GS base */
		if (copy_to_user((void *)addr,
				 &(uint64_t){ rdmsr(MSR_GS_BASE) },
				 sizeof(uint64_t)) != 0)
			return (uint64_t)-1;
		return 0;
	default:
		return (uint64_t)-1;
	}
}

static uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len) {
	return (uint64_t)vfs_write((int)fd, buf, (size_t)len);
}

static void sys_exit(int code) {
	(void)code;
	task_exit();
}

/* User heap base (virtual) - choose a safe user-space region */
#define USER_HEAP_BASE 0x40000000u
#define PAGE_SIZE 0x1000u

static uint64_t sys_sbrk(intptr_t inc) {
	task_t *t = task_current();
	if (!t) {
		printk("SBRK: no task\n");
		return (uint64_t)-1;
	}

	if (t->user_brk == 0) {
		/* initialize program break base */
		t->user_brk = (uint64_t)USER_HEAP_BASE;
		t->user_brk_size = 0;
		printk("SBRK: init heap at 0x%lx\n",
		       (unsigned long)USER_HEAP_BASE);
	}

	uint64_t current_brk = t->user_brk + t->user_brk_size;

	if (inc == 0) {
		/* return current program break */
		printk("SBRK(0): ret=0x%lx\n", (unsigned long)current_brk);
		return current_brk;
	}

	if (inc < 0) {
		/* shrinking not implemented */
		printk("SBRK: shrink not supported\n");
		return (uint64_t)-1;
	}

	printk("SBRK: brk=0x%lx inc=%ld\n", (unsigned long)current_brk,
	       (long)inc);

	uint64_t new_end = current_brk + (uint64_t)inc;

	/* Calculate the next page boundary after current_brk */
	/* If current_brk is page-aligned, start from the next page */
	/* Otherwise, start from the page boundary after current_brk */
	uint64_t first_new_page;
	if ((current_brk & (PAGE_SIZE - 1)) == 0 && t->user_brk_size > 0) {
		/* current_brk is page-aligned and we've allocated before */
		first_new_page = current_brk;
	} else {
		/* Round up to next page boundary */
		first_new_page = (current_brk + PAGE_SIZE - 1) &
				 ~(PAGE_SIZE - 1);
	}

	uint64_t new_page_end = (new_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint32_t pages = 0;
	if (new_page_end > first_new_page)
		pages = (uint32_t)((new_page_end - first_new_page) / PAGE_SIZE);

	printk("SBRK: first_new_page=0x%lx new_page_end=0x%lx\n",
	       (unsigned long)first_new_page, (unsigned long)new_page_end);

	if (pages == 0) {
		/* no page boundary crossed, just increase size */
		t->user_brk_size += (uint64_t)inc;
		printk("SBRK: no new pages needed, old=0x%lx\n",
		       (unsigned long)current_brk);
		return current_brk;
	}

	printk("SBRK: allocating %u pages\n", pages);

	/* allocate all frames first */
	uint64_t *allocated_phys =
		(uint64_t *)kmalloc(pages * sizeof(uint64_t));
	if (!allocated_phys) {
		printk("SBRK: kmalloc failed\n");
		return (uint64_t)-1;
	}

	for (uint32_t i = 0; i < pages; ++i) {
		void *frm = alloc_frame();
		if (!frm) {
			printk("SBRK: alloc_frame failed at page %u\n", i);
			/* allocation failed - free previously allocated frames */
			for (uint32_t j = 0; j < i; ++j)
				free_frame(
					(void *)(uintptr_t)allocated_phys[j]);
			kfree(allocated_phys);
			return (uint64_t)-1;
		}
		allocated_phys[i] = (uint64_t)(uintptr_t)frm;

		/* Zero out the new page */
		char *page_ptr = (char *)(uintptr_t)frm;
		for (uint32_t j = 0; j < PAGE_SIZE; ++j)
			page_ptr[j] = 0;
	}

	/* map frames into the task page directory */
	uint64_t va = first_new_page;
	int map_failed = 0;
	printk("SBRK: mapping %u pages starting at va=0x%lx\n", pages,
	       (unsigned long)va);
	for (uint32_t i = 0; i < pages; ++i, va += PAGE_SIZE) {
		/* Use map_page_64 for 64-bit address space */
		if (map_page_64(t->page_directory, allocated_phys[i], va,
				PAGING_PRESENT | PAGING_RW | PAGING_USER) !=
		    0) {
			printk("SBRK: map_page_64 failed at va=0x%lx\n",
			       (unsigned long)va);
			map_failed = 1;
			/* free remaining allocated frames */
			for (uint32_t j = i; j < pages; ++j)
				free_frame(
					(void *)(uintptr_t)allocated_phys[j]);
			/* TODO: unmap already-mapped pages (requires unmap_page_64) */
			/* For now, just free the physical frames */
			for (uint32_t j = 0; j < i; ++j) {
				free_frame(
					(void *)(uintptr_t)allocated_phys[j]);
			}
			break;
		}
	}

	kfree(allocated_phys);

	if (map_failed)
		return (uint64_t)-1;

	/* success - bump break size and return old break */
	uint64_t old_brk = current_brk;
	t->user_brk_size = new_end - t->user_brk;
	printk("SBRK: OK old=0x%lx new=0x%lx pages=%u\n",
	       (unsigned long)old_brk, (unsigned long)new_end, pages);
	return old_brk;
}

static uint64_t sys_read(uint64_t fd, void *buf, uint64_t len) {
	return (uint64_t)vfs_read((int)fd, buf, (size_t)len);
}

static uint64_t sys_close(uint64_t fd) {
	return (uint64_t)vfs_close((int)fd);
}

static uint64_t sys_open(const char *pathname, uint64_t flags, uint64_t mode) {
	return (uint64_t)vfs_open(pathname, (int)flags, (int)mode);
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
	return (uint64_t)vfs_lseek((int)fd, (int64_t)offset, (int)whence);
}

static uint64_t sys_isatty(uint64_t fd) {
	return (uint64_t)vfs_isatty((int)fd);
}

static uint64_t sys_fstat(uint64_t fd, void *buf) {
	return (uint64_t)vfs_fstat((int)fd, buf);
}

static uint64_t sys_get_reent(uint64_t size) {
	if (size == 0 || size > 4096) {
		/* guard: limit single allocation to one page */
		return (uint64_t)-1;
	}
	void *p = kmalloc((uint32_t)size);
	if (!p)
		return (uint64_t)-1;
	uint8_t *bp = (uint8_t *)p;
	for (uint32_t i = 0; i < (uint32_t)size; ++i)
		bp[i] = 0;
	return (uint64_t)p;
}

static uint64_t sys_getpid(void) {
	task_t *t = task_current();
	if (!t)
		return (uint64_t)0;
	return (uint64_t)t->tid;
}

static uint64_t sys_kill(uint64_t pid, uint64_t sig) {
	(void)sig;
	/* Minimal implementation: accept any pid and return success (0).
	 * Full implementation would locate task by TID and signal it.
	 */
	(void)pid;
	return 0;
}

static uint64_t dispatch_syscall(uint64_t num, uint64_t a0, uint64_t a1,
				 uint64_t a2, uint64_t a3, uint64_t a4,
				 uint64_t a5) {
	(void)a1;
	(void)a2;
	(void)a3;
	(void)a4;
	(void)a5; /* suppress unused warnings */
	printk("SYSCALL num=");
	printk("%llu", (unsigned long long)num);
	printk("\n");
	switch (num) {
	case SYS_write:
		return sys_write(a0, (const void *)a1, a2);
	case SYS_read:
		return sys_read(a0, (void *)a1, a2);
	case SYS_close:
		return sys_close(a0);
	case SYS_open:
		return sys_open((const char *)a0, a1, a2);
	case SYS_lseek:
		return sys_lseek(a0, a1, a2);
	case SYS_isatty:
		return sys_isatty(a0);
	case SYS_fstat:
		return sys_fstat(a0, (void *)a1);
	case SYS_exit:
		sys_exit((int)a0);
		return 0;
	case SYS_sbrk:
		return sys_sbrk((intptr_t)a0);
	case SYS_get_reent:
		return sys_get_reent(a0);
	case SYS_getpid:
		return sys_getpid();
	case SYS_kill:
		return sys_kill(a0, a1);
	case 158: /* arch_prctl */
		return sys_arch_prctl((int)a0, a1);
	default:
		return (uint64_t)-1; /* ENOSYS */
	}
}

void syscall_entry_c(uint64_t *regs_stack, uint32_t vec) {
	(void)vec;
	uint64_t num = regs_stack[0];

	/* Debug: Print syscall info */
	// printk("SYSCALL: num=%llu rdi=%llx rsi=%llx rdx=%llx\n",
	//        (unsigned long long)num, (unsigned long long)regs_stack[6],
	//        (unsigned long long)regs_stack[5], (unsigned long long)regs_stack[2]);

	/* some registers are not used by our syscall dispatch; suppress warnings */
	(void)vec;
	uint64_t rax = regs_stack[0];
	(void)rax;
	uint64_t rcx = regs_stack[1];
	(void)rcx;
	uint64_t rdx = regs_stack[2];
	(void)rdx;
	uint64_t rbx = regs_stack[3];
	(void)rbx;
	uint64_t rbp = regs_stack[4];
	(void)rbp;
	uint64_t rsi = regs_stack[5];
	uint64_t rdi = regs_stack[6];
	uint64_t r8 = regs_stack[7];
	uint64_t r9 = regs_stack[8];
	uint64_t r10 = regs_stack[9];

	uint64_t a0 = rdi;
	uint64_t a1 = rsi;
	uint64_t a2 = rdx;
	uint64_t a3 = r10;
	(void)a3;
	uint64_t a4 = r8;
	(void)a4;
	uint64_t a5 = r9;
	(void)a5;

	uint64_t ret = dispatch_syscall(num, a0, a1, a2, a3, a4, a5);

	regs_stack[0] = ret;

	// printk("SYSCALL: returning ret=%llx\n", (unsigned long long)ret);
}

/* syscall instruction handler (defined in syscall_handler.asm) */
extern void syscall_handler(void);

/**
 * Initialize SYSCALL instruction support
 * Sets up MSRs for fast system calls
 */
void syscall_init(void) {
	uint64_t efer;

	/* Enable SYSCALL/SYSRET instructions */
	efer = rdmsr(MSR_EFER);
	efer |= EFER_SCE;
	wrmsr(MSR_EFER, efer);

	/* Set up STAR MSR: CS/SS selectors
	 * New GDT layout (for SYSRET compatibility):
	 *   0x00: NULL
	 *   0x08: Kernel Code (64-bit)
	 *   0x10: Kernel Data
	 *   0x18: User Code (32-bit) - for SYSRET
	 *   0x20: User Data
	 *   0x28: User Code (64-bit) - actual 64-bit user code
	 * 
	 * SYSCALL behavior:
	 *   CS = STAR[47:32]
	 *   SS = STAR[47:32] + 8
	 * 
	 * SYSRET behavior:
	 *   CS = (STAR[63:48] + 16) | 3  => Should be 0x2B (0x28 + 3)
	 *   SS = (STAR[63:48] + 8) | 3   => Should be 0x23 (0x20 + 3)
	 * 
	 * For CS = (STAR[63:48] + 16) | 3 = 0x2B:
	 *   STAR[63:48] + 16 = 0x28
	 *   STAR[63:48] = 0x18
	 * 
	 * Verify SS = (0x18 + 8) | 3 = 0x20 | 3 = 0x23 ✓
	 * 
	 * STAR = (0x18 << 48) | (0x08 << 32)
	 */
	uint64_t star = ((uint64_t)0x18 << 48) | ((uint64_t)0x08 << 32);
	wrmsr(MSR_STAR, star);

	/* Set up LSTAR: syscall entry point */
	wrmsr(MSR_LSTAR, (uint64_t)syscall_handler);

	/* Set up SFMASK: RFLAGS mask (clear IF to disable interrupts) */
	wrmsr(MSR_SFMASK, 0x200); /* Clear IF flag during syscall */

	printk("syscall: SYSCALL instruction support enabled\n");
}
