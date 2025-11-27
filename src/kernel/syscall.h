#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

#define SYS_write 1
#define SYS_exit 2
#define SYS_sbrk 3
#define SYS_read 4
#define SYS_close 5
#define SYS_fstat 6
#define SYS_lseek 7
#define SYS_open 8
#define SYS_isatty 9
#define SYS_get_reent 10
#define SYS_getpid 11
#define SYS_kill 12

void syscall_entry_c(uint64_t *regs_stack, uint32_t vec);

#endif /* _KERNEL_SYSCALL_H */