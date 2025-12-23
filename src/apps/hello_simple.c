// Minimal test program for LiteCore - direct syscalls only
// No libc dependencies

// Write syscall (1)
static inline long write_syscall(int fd, const char *buf, unsigned long count) {
	long ret;
	asm volatile("int $0x80"
		     : "=a"(ret)
		     : "a"(1), "D"(fd), "S"(buf), "d"(count)
		     : "rcx", "r11", "memory");
	return ret;
}

// Exit syscall (2)
static inline void exit_syscall(int code) {
	asm volatile("int $0x80"
		     :
		     : "a"(2), "D"(code)
		     : "rcx", "r11", "memory");
	__builtin_unreachable();
}

void _start(void) {
	const char msg[] = "Hello from simple app!\n";
	write_syscall(1, msg, sizeof(msg) - 1);
	exit_syscall(0);

	// Fallback: infinite loop instead of hlt
	while (1) {
		asm volatile("pause");
	}
}
