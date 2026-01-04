#include <proc/proc.h>
#include <stddef.h>
#include <string.h>

#define PROC_MAX 64
#define CWD_MAX 256

struct proc_entry {
	uint32_t pid;
	char cwd[CWD_MAX];
};

static struct proc_entry table[PROC_MAX];

void proc_init(void) {
	for (int i = 0; i < PROC_MAX; ++i) {
		table[i].pid = 0;
		table[i].cwd[0] = '/';
		table[i].cwd[1] = '\0';
	}
}

static struct proc_entry *find_entry(uint32_t pid) {
	for (int i = 0; i < PROC_MAX; ++i) {
		if (table[i].pid == pid)
			return &table[i];
	}
	return NULL;
}

int proc_create(uint32_t pid) {
	if (pid == 0) {
		/* ensure slot 0 reserved if available */
		if (table[0].pid == 0) {
			table[0].pid = pid;
			table[0].cwd[0] = '/';
			table[0].cwd[1] = '\0';
			return 0;
		}
	}
	for (int i = 0; i < PROC_MAX; ++i) {
		if (table[i].pid == 0) {
			table[i].pid = pid;
			table[i].cwd[0] = '/';
			table[i].cwd[1] = '\0';
			return 0;
		}
	}
	return -1;
}

void proc_remove(uint32_t pid) {
	struct proc_entry *e = find_entry(pid);
	if (!e)
		return;
	e->pid = 0;
	e->cwd[0] = '/';
	e->cwd[1] = '\0';
}

int proc_set_cwd(uint32_t pid, const char *path) {
	if (!path)
		return -1;
	struct proc_entry *e = find_entry(pid);
	if (!e)
		return -1;
	/* copy and ensure null terminated */
	int i = 0;
	for (; i < CWD_MAX - 1 && path[i]; ++i)
		e->cwd[i] = path[i];
	e->cwd[i] = '\0';
	return 0;
}

const char *proc_get_cwd(uint32_t pid) {
	struct proc_entry *e = find_entry(pid);
	if (!e)
		return NULL;
	return e->cwd;
}
