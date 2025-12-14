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
		return (uint64_t)-1;
	}

	if (t->user_brk == 0) {
		/* initialize program break base */
		t->user_brk = (uint64_t)USER_HEAP_BASE;
		t->user_brk_size = 0;
	}

	uint64_t current_brk = t->user_brk + t->user_brk_size;

	if (inc == 0) {
		/* return current program break */
		return current_brk;
	}

	if (inc < 0) {
		/* shrinking not implemented */
		return (uint64_t)-1;
	}

	uint64_t new_end = current_brk + (uint64_t)inc;

	uint32_t prev_page = (uint32_t)(current_brk & ~(PAGE_SIZE - 1));
	uint32_t new_page_end =
		(uint32_t)((new_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

	uint32_t pages = 0;
	if (new_page_end > prev_page)
		pages = (new_page_end - prev_page) / PAGE_SIZE;

	if (pages == 0) {
		/* no page boundary crossed, just increase size */
		t->user_brk_size += (uint64_t)inc;
		return current_brk;
	}

	/* allocate all frames first */
	uint32_t *allocated_phys =
		(uint32_t *)kmalloc(pages * sizeof(uint32_t));
	if (!allocated_phys)
		return (uint64_t)-1;

	for (uint32_t i = 0; i < pages; ++i) {
		void *frm = alloc_frame();
		if (!frm) {
			/* allocation failed - free previously allocated frames */
			for (uint32_t j = 0; j < i; ++j)
				free_frame(
					(void *)(uintptr_t)allocated_phys[j]);
			kfree(allocated_phys);
			return (uint64_t)-1;
		}
		allocated_phys[i] = (uint32_t)(uintptr_t)frm;
	}

	/* map frames into the task page directory */
	uint32_t va = prev_page;
	int map_failed = 0;
	for (uint32_t i = 0; i < pages; ++i, va += PAGE_SIZE) {
		if (map_page_pd((uint32_t)t->page_directory, allocated_phys[i],
				va, PAGING_PRESENT | PAGING_RW | PAGING_USER) !=
		    0) {
			map_failed = 1;
			/* free remaining allocated frames */
			for (uint32_t j = i; j < pages; ++j)
				free_frame(
					(void *)(uintptr_t)allocated_phys[j]);
			/* unmap and free already-mapped frames */
			uint32_t un_va = prev_page;
			for (uint32_t j = 0; j < i; ++j, un_va += PAGE_SIZE) {
				unmap_page_pd((uint32_t)t->page_directory,
					      un_va);
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
	t->user_brk_size = (uint64_t)(new_page_end - (uint32_t)t->user_brk) +
			   (new_end - new_page_end);
	/* simpler: set size to new_end - base */
	t->user_brk_size = new_end - t->user_brk;
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
}
