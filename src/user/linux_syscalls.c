/* Linux-compatible syscalls wrapper for testing apps on host Linux */
/* Also works on LiteCore kernel with Linux-compatible syscall numbers */
#include <sys/types.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdint.h>

/* Linux syscall numbers (also used by LiteCore) */
#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_brk 12
#define SYS_getpid 39
#define SYS_exit 60
#define SYS_kill 62
#define SYS_isatty 100 /* Custom for LiteCore */
#define SYS_arch_prctl 158
#define SYS_get_reent 200 /* Custom LiteCore syscall */

extern int errno;

static inline long linux_syscall6(long n, long a1, long a2, long a3, long a4,
				  long a5, long a6) {
	register long rax asm("rax") = n;
	register long rdi asm("rdi") = a1;
	register long rsi asm("rsi") = a2;
	register long rdx asm("rdx") = a3;
	register long r10 asm("r10") = a4;
	register long r8 asm("r8") = a5;
	register long r9 asm("r9") = a6;
	asm volatile("syscall"
		     : "+a"(rax), "+c"(rdi), "+S"(rsi), "+d"(rdx), "+r"(r10),
		       "+r"(r8), "+r"(r9)
		     :
		     : "r11", "memory");
	return rax;
}

ssize_t write(int fd, const void *buf, size_t count) {
	long r = linux_syscall6(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (ssize_t)r;
}

ssize_t _write(int fd, const void *buf, size_t count) {
	return write(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {
	long r = linux_syscall6(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (ssize_t)r;
}

ssize_t _read(int fd, void *buf, size_t count) {
	return read(fd, buf, count);
}

int close(int fd) {
	long r = linux_syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}

int _close(int fd) {
	return close(fd);
}

int fstat(int fd, struct stat *st) {
	long r = linux_syscall6(SYS_fstat, fd, (long)st, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return 0;
}

int _fstat(int fd, struct stat *st) {
	return fstat(fd, st);
}

int isatty(int fd) {
	/* Use custom LiteCore syscall if available, otherwise simple implementation */
	long r = linux_syscall6(SYS_isatty, fd, 0, 0, 0, 0, 0);
	if (r >= 0)
		return (int)r;
	return 1; /* Fallback */
}

int _isatty(int fd) {
	return isatty(fd);
}

off_t lseek(int fd, off_t offset, int whence) {
	long r = linux_syscall6(SYS_lseek, fd, (long)offset, (long)whence, 0, 0,
				0);
	if (r < 0) {
		errno = (int)-r;
		return (off_t)-1;
	}
	return (off_t)r;
}

off_t _lseek(int fd, off_t offset, int whence) {
	return lseek(fd, offset, whence);
}

int open(const char *pathname, int flags, int mode) {
	long r = linux_syscall6(SYS_open, (long)pathname, flags, mode, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}

int _open(const char *pathname, int flags, int mode) {
	return open(pathname, flags, mode);
}

void _exit(int status) {
	linux_syscall6(SYS_exit, status, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static void *current_brk = 0;

void *sbrk(ptrdiff_t increment) {
	if (!current_brk) {
		/* Get current brk */
		long r = linux_syscall6(SYS_brk, 0, 0, 0, 0, 0, 0);
		if (r < 0) {
			errno = 12; /* ENOMEM */
			return (void *)-1;
		}
		current_brk = (void *)r;
	}

	if (increment == 0) {
		return current_brk;
	}

	void *old_brk = current_brk;
	void *new_brk = (void *)((char *)old_brk + increment);

	long r = linux_syscall6(SYS_brk, (long)new_brk, 0, 0, 0, 0, 0);
	if (r < 0 || (void *)r < new_brk) {
		errno = 12; /* ENOMEM */
		return (void *)-1;
	}

	current_brk = (void *)r;
	return old_brk;
}

void *_sbrk(ptrdiff_t increment) {
	return sbrk(increment);
}

int getpid(void) {
	return (int)linux_syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0);
}

int _getpid(void) {
	return getpid();
}

int kill(int pid, int sig) {
	long r = linux_syscall6(SYS_kill, pid, sig, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return 0;
}

int _kill(int pid, int sig) {
	return kill(pid, sig);
}

/* LiteCore-specific syscalls */
__attribute__((constructor)) static void _newlib_reent_init(void) {
	/* Try to use LiteCore's custom get_reent syscall */
	const long alloc_size = 4096;
	long r = linux_syscall6(SYS_get_reent, alloc_size, 0, 0, 0, 0, 0);
	if (r == -1)
		return; /* Not on LiteCore or allocation failed */

	/* On LiteCore, set up _impure_ptr */
	extern char _impure_ptr;
	*(struct _reent **)&_impure_ptr = (struct _reent *)(uintptr_t)r;
}
