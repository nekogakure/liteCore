#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "list.h"

extern char current_path[256];

int cmd_cd(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: cd <directory>\n");
		return -1;
	}
	if (chdir(argv[1]) == 0) {
		if (getcwd(current_path, sizeof(current_path)) == NULL) {
			strcpy(current_path, argv[1]);
		}
		return 0;
	} else {
		printf("cd: %s: No such directory\n", argv[1]);
		return -1;
	}
}
