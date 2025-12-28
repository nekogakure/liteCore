#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_sbrk 12
#define SYS_getpid 39
#define SYS_exit 60
#define SYS_kill 62
#define SYS_isatty 100
#define SYS_arch_prctl 158
#define SYS_get_reent 200
#define SYS_fork 201
#define SYS_execve 202
#define SYS_waitpid 203
#define SYS_mmap 209
#define SYS_munmap 210
#define SYS_mprotect 211

void syscall_entry_c(uint64_t *regs_stack, uint32_t vec);
void syscall_init(void);

#endif /* _KERNEL_SYSCALL_H */