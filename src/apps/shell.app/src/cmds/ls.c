#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <core/sys.h>
#include "list.h"

extern char current_path[256];

int cmd_ls(int argc, char **argv) {
	(void)argc;
	(void)argv;
	/* Use kernel-backed listing helper (prints via kernel console) */
	if (listdir(current_path) != 0) {
		printf("ls: cannot list directory\n");
		return -1;
	}
	return 0;
}
