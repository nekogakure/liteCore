#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef S_IFCHR
#define S_IFCHR 0020000
#endif

enum {
	/* Use x86_64 syscall numbers */
	SYS_read = 0,
	SYS_write = 1,
	SYS_open = 2,
	SYS_close = 3,
	SYS_fstat = 5,
	SYS_lseek = 8,
	SYS_brk = 12,
	SYS_getpid = 39,
	SYS_kill = 62,
	SYS_exit = 60,
};

int __errno = 0;
int *__errno_location(void) {
	return &__errno;
}

static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5,
			    long a6) {
	register long rax asm("rax") = n;
	register long rdi asm("rdi") = a1;
	register long rsi asm("rsi") = a2;
	register long rdx asm("rdx") = a3;
	register long r10 asm("r10") = a4;
	register long r8 asm("r8") = a5;
	register long r9 asm("r9") = a6;
	asm volatile("syscall"
		     : "+a"(rax)
		     : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
		     : "rcx", "r11", "memory");
	return rax;
}

ssize_t _write(int fd, const void *buf, size_t count) {
	long r = syscall6(SYS_write, fd, (long)buf, (long)count, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return r;
}

void _exit(int status) {
	syscall6(SYS_exit, status, 0, 0, 0, 0, 0);
	while (1) {
	}
}

void *_sbrk(ptrdiff_t increment) {
	/* brk(0) returns current program break; then set to current+increment */
	long cur = syscall6(SYS_brk, 0, 0, 0, 0, 0, 0);
	if (cur == (long)-1) {
		errno = ENOMEM;
		return (void *)-1;
	}
	long new = syscall6(SYS_brk, cur + increment, 0, 0, 0, 0, 0);
	if (new == (long)-1) {
		errno = ENOMEM;
		return (void *)-1;
	}
	return (void *)cur;
}

ssize_t write(int fd, const void *buf, size_t count) {
	return _write(fd, buf, count);
}

ssize_t read(int fd, void *buf, size_t count) {
	long r = syscall6(SYS_read, fd, (long)buf, (long)count, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return r;
}

int close(int fd) {
	long r = syscall6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}

off_t lseek(int fd, off_t offset, int whence) {
	long r = syscall6(SYS_lseek, fd, (long)offset, (long)whence, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (off_t)r;
}

int open(const char *pathname, int flags, ...) {
	long r = syscall6(SYS_open, (long)pathname, flags, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}

int isatty(int fd) {
	return (fd == 1 || fd == 2) ? 1 : 0;
}

int fstat(int fd, struct stat *st) {
	if (!st) {
		errno = EINVAL;
		return -1;
	}
	if (fd == 1 || fd == 2) {
		long r = syscall6(SYS_fstat, fd, (long)st, 0, 0, 0, 0);
		if (r < 0) {
			errno = (int)-r;
			return -1;
		}
		return 0;
	}
	errno = ENOSYS;
	return -1;
}

void *sbrk(ptrdiff_t increment) {
	return _sbrk(increment);
}

int getpid(void) {
	long r = syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0);
	return (int)r;
}

int kill(int pid, int sig) {
	long r = syscall6(SYS_kill, pid, sig, 0, 0, 0, 0);
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return (int)r;
}