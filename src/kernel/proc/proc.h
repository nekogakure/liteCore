/* Simple process table for per-process state (cwd, etc.) */
#ifndef _KERNEL_PROC_H
#define _KERNEL_PROC_H

#include <stdint.h>

void proc_init(void);
int proc_create(uint32_t pid);
void proc_remove(uint32_t pid);
int proc_set_cwd(uint32_t pid, const char *path);
const char *proc_get_cwd(uint32_t pid);

#endif
