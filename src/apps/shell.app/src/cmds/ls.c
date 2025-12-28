#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "list.h"

extern char current_path[256];

int cmd_ls(int argc, char **argv) {
	(void)argc;
	(void)argv;
	DIR *dir = opendir(current_path);
	if (!dir) {
		printf("ls: cannot open directory\n");
		return -1;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		printf("%s  ", entry->d_name);
	}
	printf("\n");
	closedir(dir);
	return 0;
}
